// MIT License
//
// Copyright(c) 2021-2022 Matthieu Bucchianeri
// Copyright(c) 2021-2022 Jean-Luc Dupiot - Reality XP
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "pch.h"

#include "factories.h"
#include "interfaces.h"
#include "layer.h"
#include "log.h"

namespace {

    // 2 views to process, one per eye.
    constexpr uint32_t ViewCount = 2;

    // The xrWaitFrame() loop might cause to have 2 frames in-flight, so we want to delay the GPU timer re-use by those
    // 2 frames.
    constexpr uint32_t GpuTimerLatency = 2;

    using namespace toolkit;
    using namespace toolkit::log;

    using namespace xr::math;

    struct SwapchainImages {
        std::vector<std::shared_ptr<graphics::ITexture>> chain;

        std::shared_ptr<graphics::IGpuTimer> upscalerGpuTimer[ViewCount];
        std::shared_ptr<graphics::IGpuTimer> preProcessorGpuTimer[ViewCount];
        std::shared_ptr<graphics::IGpuTimer> postProcessorGpuTimer[ViewCount];
    };

    struct SwapchainState {
        std::vector<SwapchainImages> images;
        uint32_t acquiredImageIndex{0};
    };

    class OpenXrLayer : public toolkit::OpenXrApi {
      public:
        OpenXrLayer() = default;
        ~OpenXrLayer() override = default;

        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            // Needed to resolve the requested function pointers.
            OpenXrApi::xrCreateInstance(createInfo);

            // TODO: This should be auto-generated in the call above, but today our generator only looks at core spec.
            // We may let this fail intentionally and check that the pointer is populated later.
            xrGetInstanceProcAddr(GetXrInstance(),
                                  "xrConvertWin32PerformanceCounterToTimeKHR",
                                  reinterpret_cast<PFN_xrVoidFunction*>(&xrConvertWin32PerformanceCounterToTimeKHR));

            m_applicationName = createInfo->applicationInfo.applicationName;

            // Dump the OpenXR runtime information to help debugging customer issues.
            XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
            CHECK_XRCMD(xrGetInstanceProperties(GetXrInstance(), &instanceProperties));
            m_runtimeName = fmt::format("{} {}.{}.{}",
                                        instanceProperties.runtimeName,
                                        XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                        XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                        XR_VERSION_PATCH(instanceProperties.runtimeVersion));
            Log("Using OpenXR runtime %s\n", m_runtimeName.c_str());

            m_configManager = config::CreateConfigManager(createInfo->applicationInfo.applicationName);

            // We must initialize hand tracking early on, because the application can start creating actions etc
            // before creating the session.
            m_configManager->setEnumDefault(config::SettingHandTrackingEnabled, config::HandTrackingEnabled::Off);
            m_configManager->setDefault(config::SettingHandVisibilityAndSkinTone, 2); // Visible - Medium
            if (m_configManager->getEnumValue<config::HandTrackingEnabled>(config::SettingHandTrackingEnabled) !=
                config::HandTrackingEnabled::Off) {
                m_handTracker = input::CreateHandTracker(*this, m_configManager);
                m_sendInterationProfileEvent = true;
            }

            return XR_SUCCESS;
        }

        XrResult xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) override {
            const XrResult result = OpenXrApi::xrGetSystem(instance, getInfo, systemId);
            if (XR_SUCCEEDED(result) && getInfo->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
                // Store the actual OpenXR resolution.
                XrViewConfigurationView views[ViewCount] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                                            {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
                uint32_t viewCount;
                CHECK_XRCMD(OpenXrApi::xrEnumerateViewConfigurationViews(
                    instance, *systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, ViewCount, &viewCount, views));

                m_displayWidth = views[0].recommendedImageRectWidth;
                m_displayHeight = views[0].recommendedImageRectHeight;

                // Check for hand tracking support.
                XrSystemHandTrackingPropertiesEXT handTrackingSystemProperties{
                    XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
                handTrackingSystemProperties.supportsHandTracking = false;
                XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES, &handTrackingSystemProperties};
                OpenXrApi::xrGetSystemProperties(instance, *systemId, &systemProperties);
                m_supportHandTracking = handTrackingSystemProperties.supportsHandTracking;

                // Workaround: the WMR runtime supports something called the XR_MSFT_hand_interaction, which will
                // (falsely) advertise hand tracking support (in reality hand tracking API support from the controller's
                // input). Check for the Ultraleap layer in this case.
                if (m_runtimeName.find("Windows Mixed Reality Runtime") != std::string::npos) {
                    bool hasUltraleapLayer = false;
                    for (const auto& layer : GetUpstreamLayers()) {
                        if (layer == "XR_APILAYER_ULTRALEAP_hand_tracking") {
                            hasUltraleapLayer = true;
                        }
                    }
                    if (!hasUltraleapLayer) {
                        Log("Ignoring XR_MSFT_hand_interaction for %s\n", m_runtimeName.c_str());
                        m_supportHandTracking = false;
                    }
                }

                // We had to initialize the hand tracker early on. If we find out now that hand tracking is not
                // supported, then destroy it. This could happen if the option was set while a hand tracking device was
                // connected, but later the hand tracking device was disconnected.
                if (!m_supportHandTracking) {
                    m_handTracker.reset();
                }

                // Set the default settings.
                m_configManager->setEnumDefault(config::SettingScalingType, config::ScalingType::None);
                m_configManager->setDefault(config::SettingScaling, 100);
                m_configManager->setDefault(config::SettingSharpness, 20);
                m_configManager->setDefault(config::SettingFOV, 100);
                m_configManager->setDefault(config::SettingPredictionDampen, 100);

                // Remember the XrSystemId to use.
                m_vrSystemId = *systemId;
            }

            return result;
        }

        XrResult xrEnumerateViewConfigurationViews(XrInstance instance,
                                                   XrSystemId systemId,
                                                   XrViewConfigurationType viewConfigurationType,
                                                   uint32_t viewCapacityInput,
                                                   uint32_t* viewCountOutput,
                                                   XrViewConfigurationView* views) override {
            const XrResult result = OpenXrApi::xrEnumerateViewConfigurationViews(
                instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views);
            if (XR_SUCCEEDED(result) && isVrSystem(systemId) && views) {
                // Determine the application resolution.
                const auto upscaleMode = m_configManager->getEnumValue<config::ScalingType>(config::SettingScalingType);

                uint32_t inputWidth = m_displayWidth;
                uint32_t inputHeight = m_displayHeight;

                switch (upscaleMode) {
                case config::ScalingType::FSR:
                    [[fallthrough]];

                case config::ScalingType::NIS:
                    std::tie(inputWidth, inputHeight) = utilities::GetScaledDimensions(
                        m_displayWidth, m_displayHeight, m_configManager->getValue(config::SettingScaling), 2);
                    break;

                case config::ScalingType::None:
                    break;

                default:
                    throw new std::runtime_error("Unknown scaling type");
                    break;
                }

                if (inputWidth != m_displayWidth || inputHeight != m_displayHeight) {
                    // Override the recommended image size to account for scaling.
                    for (uint32_t i = 0; i < *viewCountOutput; i++) {
                        views[i].recommendedImageRectWidth = inputWidth;
                        views[i].recommendedImageRectHeight = inputHeight;

                        if (i == 0) {
                            Log("Upscaling from %ux%u to %ux%u (%u%%)\n",
                                views[i].recommendedImageRectWidth,
                                views[i].recommendedImageRectHeight,
                                m_displayWidth,
                                m_displayHeight,
                                (unsigned int)((((float)m_displayWidth / views[i].recommendedImageRectWidth) + 0.001f) *
                                               100));
                        }
                    }
                } else {
                    Log("Using OpenXR resolution (no upscaling): %ux%u\n", m_displayWidth, m_displayHeight);
                }
            }

            return result;
        }

        XrResult xrCreateSession(XrInstance instance,
                                 const XrSessionCreateInfo* createInfo,
                                 XrSession* session) override {
            const XrResult result = OpenXrApi::xrCreateSession(instance, createInfo, session);
            if (XR_SUCCEEDED(result) && isVrSystem(createInfo->systemId)) {
                // Get the graphics device.
                const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
                while (entry) {
                    if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                        const XrGraphicsBindingD3D11KHR* d3dBindings =
                            reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry);
                        m_graphicsDevice = graphics::WrapD3D11Device(d3dBindings->device);
                        break;
                    } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                        const XrGraphicsBindingD3D12KHR* d3dBindings =
                            reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
                        m_graphicsDevice = graphics::WrapD3D12Device(d3dBindings->device, d3dBindings->queue);
                        break;
                    }

                    entry = entry->next;
                }

                if (m_graphicsDevice) {
                    // Initialize the other resources.
                    m_upscaleMode = m_configManager->getEnumValue<config::ScalingType>(config::SettingScalingType);

                    switch (m_upscaleMode) {
                    case config::ScalingType::FSR:
                        m_upscaler = graphics::CreateFSRUpscaler(
                            m_configManager, m_graphicsDevice, m_displayWidth, m_displayHeight);

                        // Latch this value now.
                        m_upscalingFactor = m_configManager->getValue(config::SettingScaling);
                        break;

                    case config::ScalingType::NIS:
                        m_upscaler = graphics::CreateNISUpscaler(
                            m_configManager, m_graphicsDevice, m_displayWidth, m_displayHeight);

                        // Latch this value now.
                        m_upscalingFactor = m_configManager->getValue(config::SettingScaling);
                        break;

                    case config::ScalingType::None:
                        break;

                    default:
                        throw new std::runtime_error("Unknown scaling type");
                        break;
                    }

                    m_postProcessor =
                        graphics::CreateImageProcessor(m_configManager, m_graphicsDevice, "postprocess.hlsl");

                    m_performanceCounters.appCpuTimer = utilities::CreateCpuTimer();
                    m_performanceCounters.endFrameCpuTimer = utilities::CreateCpuTimer();
                    m_performanceCounters.overlayCpuTimer = utilities::CreateCpuTimer();

                    for (unsigned int i = 0; i <= GpuTimerLatency; i++) {
                        m_performanceCounters.appGpuTimer[i] = m_graphicsDevice->createTimer();
                        m_performanceCounters.overlayGpuTimer[i] = m_graphicsDevice->createTimer();
                    }

                    m_performanceCounters.lastWindowStart = std::chrono::steady_clock::now();

                    m_menuHandler = menu::CreateMenuHandler(m_configManager,
                                                            m_graphicsDevice,
                                                            m_displayWidth,
                                                            m_displayHeight,
                                                            m_supportHandTracking,
                                                            xrConvertWin32PerformanceCounterToTimeKHR != nullptr);
                } else {
                    Log("Unsupported graphics runtime.\n");
                }

                if (m_handTracker) {
                    m_handTracker->beginSession(*session, m_graphicsDevice);
                }

                // Remember the XrSession to use.
                m_vrSession = *session;
            }

            return result;
        }

        XrResult xrDestroySession(XrSession session) override {
            const XrResult result = OpenXrApi::xrDestroySession(session);
            if (XR_SUCCEEDED(result) && isVrSession(session)) {
                // Wait for any pending operation to complete.
                if (m_graphicsDevice) {
                    m_graphicsDevice->flushContext(true);
                }

                if (m_handTracker) {
                    m_handTracker->endSession();
                }
                m_upscaler.reset();
                m_preProcessor.reset();
                m_postProcessor.reset();
                for (unsigned int i = 0; i <= GpuTimerLatency; i++) {
                    m_performanceCounters.appGpuTimer[i].reset();
                    m_performanceCounters.overlayGpuTimer[i].reset();
                }
                m_performanceCounters.appCpuTimer.reset();
                m_performanceCounters.endFrameCpuTimer.reset();
                m_performanceCounters.overlayCpuTimer.reset();
                m_swapchains.clear();
                m_menuHandler.reset();
                m_graphicsDevice->shutdown();
                m_graphicsDevice.reset();
                m_vrSession = XR_NULL_HANDLE;
                // A good check to ensure there are no resources leak is to confirm that the graphics device is
                // destroyed _before_ we see this message.
                // eg:
                // 2022-01-01 17:15:35 -0800: D3D11Device destroyed
                // 2022-01-01 17:15:35 -0800: Session destroyed
                // If the order is reversed or the Device is destructed missing, then it means that we are not cleaning
                // up the resources properly.
                Log("Session destroyed\n");
            }

            return result;
        }

        XrResult xrCreateSwapchain(XrSession session,
                                   const XrSwapchainCreateInfo* createInfo,
                                   XrSwapchain* swapchain) override {
            if (!isVrSession(session) || !m_graphicsDevice) {
                return OpenXrApi::xrCreateSwapchain(session, createInfo, swapchain);
            }

            // TODO: Identify the swapchains of interest for our processing chain. For now, we only handle color
            // buffers.
            const bool useSwapchain = createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

            Log("Creating swapchain with dimensions=%ux%u, arraySize=%u, mipCount=%u, sampleCount=%u, format=%d, "
                "usage=0x%x\n",
                createInfo->width,
                createInfo->height,
                createInfo->arraySize,
                createInfo->mipCount,
                createInfo->sampleCount,
                createInfo->format,
                createInfo->usageFlags);

            XrSwapchainCreateInfo chainCreateInfo = *createInfo;
            if (useSwapchain) {
                // TODO: Modify the swapchain to handle our processing chain (eg: change resolution and/or select usage
                // XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT).

                if (m_preProcessor) {
                    // This is redundant (given the useSwapchain conditions) but we do this for correctness.
                    chainCreateInfo.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                }

                if (m_upscaler) {
                    // When upscaling, be sure to request the full resolution with the runtime.
                    chainCreateInfo.width = m_displayWidth;
                    chainCreateInfo.height = m_displayHeight;

                    // The upscaler requires to use as an unordered access view.
                    chainCreateInfo.usageFlags |= XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
                }

                if (m_postProcessor) {
                    // We no longer need the runtime swapchain to have this flag since we will use an intermediate
                    // texture.
                    chainCreateInfo.usageFlags &= ~XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;

                    // This is redundant (given the useSwapchain conditions) but we do this for correctness.
                    chainCreateInfo.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                }
            }

            const XrResult result = OpenXrApi::xrCreateSwapchain(session, &chainCreateInfo, swapchain);
            if (XR_SUCCEEDED(result) && useSwapchain) {
                uint32_t imageCount;
                CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(*swapchain, 0, &imageCount, nullptr));

                SwapchainState swapchainState;
                if (m_graphicsDevice->getApi() == graphics::Api::D3D11) {
                    std::vector<XrSwapchainImageD3D11KHR> d3dImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                    CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(
                        *swapchain,
                        imageCount,
                        &imageCount,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(d3dImages.data())));
                    for (uint32_t i = 0; i < imageCount; i++) {
                        SwapchainImages images;

                        // Store the runtime images into the state (last entry in the processing chain).
                        images.chain.push_back(
                            graphics::WrapD3D11Texture(m_graphicsDevice,
                                                       chainCreateInfo,
                                                       d3dImages[i].texture,
                                                       fmt::format("Runtime swapchain {} TEX2D", i)));

                        swapchainState.images.push_back(images);
                    }
                } else if (m_graphicsDevice->getApi() == graphics::Api::D3D12) {
                    std::vector<XrSwapchainImageD3D12KHR> d3dImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                    CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(
                        *swapchain,
                        imageCount,
                        &imageCount,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(d3dImages.data())));
                    for (uint32_t i = 0; i < imageCount; i++) {
                        SwapchainImages images;

                        // Store the runtime images into the state (last entry in the processing chain).
                        images.chain.push_back(
                            graphics::WrapD3D12Texture(m_graphicsDevice,
                                                       chainCreateInfo,
                                                       d3dImages[i].texture,
                                                       fmt::format("Runtime swapchain {} TEX2D", i)));

                        swapchainState.images.push_back(images);
                    }
                } else {
                    throw new std::runtime_error("Unsupported graphics runtime");
                }

                for (uint32_t i = 0; i < imageCount; i++) {
                    SwapchainImages& images = swapchainState.images[i];

                    // TODO: Create other entries in the chain based on the processing to do (scaling,
                    // post-processing...).

                    if (m_preProcessor) {
                        // Create an intermediate texture with the same resolution as the input.
                        XrSwapchainCreateInfo inputCreateInfo = *createInfo;
                        inputCreateInfo.usageFlags |= XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                        if (m_upscaler) {
                            // The upscaler requires to use as a shader input.
                            inputCreateInfo.usageFlags |= XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                        }

                        auto inputTexture = m_graphicsDevice->createTexture(
                            inputCreateInfo, fmt::format("Postprocess input swapchain {} TEX2D", i));

                        // We place the texture at the very front (app texture).
                        images.chain.insert(images.chain.begin(), inputTexture);

                        images.preProcessorGpuTimer[0] = m_graphicsDevice->createTimer();
                        if (createInfo->arraySize > 1) {
                            images.preProcessorGpuTimer[1] = m_graphicsDevice->createTimer();
                        }
                    }

                    if (m_upscaler) {
                        // Create an app texture with the lower resolution.
                        XrSwapchainCreateInfo inputCreateInfo = *createInfo;
                        inputCreateInfo.usageFlags |= XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                        auto inputTexture =
                            m_graphicsDevice->createTexture(inputCreateInfo, fmt::format("App swapchain {} TEX2D", i));

                        // We place the texture before the runtime texture, which means at the very front (app
                        // texture) or after the pre-processor.
                        images.chain.insert(images.chain.end() - 1, inputTexture);

                        images.upscalerGpuTimer[0] = m_graphicsDevice->createTimer();
                        if (createInfo->arraySize > 1) {
                            images.upscalerGpuTimer[1] = m_graphicsDevice->createTimer();
                        }
                    }

                    if (m_postProcessor) {
                        // Create an intermediate texture with the same resolution as the output.
                        XrSwapchainCreateInfo intermediateCreateInfo = chainCreateInfo;
                        intermediateCreateInfo.usageFlags |= XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                        if (m_upscaler) {
                            // The upscaler requires to use as an unordered access view.
                            intermediateCreateInfo.usageFlags |= XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;

                            // This also means we need a non-sRGB type.
                            if (m_graphicsDevice->isTextureFormatSRGB(intermediateCreateInfo.format)) {
                                // good balance between visuals and perf
                                intermediateCreateInfo.format =
                                    m_graphicsDevice->getTextureFormat(graphics::TextureFormat::R10G10B10A2_UNORM);
                            }
                        }
                        auto intermediateTexture = m_graphicsDevice->createTexture(
                            intermediateCreateInfo, fmt::format("Postprocess input swapchain {} TEX2D", i));

                        // We place the texture just before the runtime texture.
                        images.chain.insert(images.chain.end() - 1, intermediateTexture);

                        images.postProcessorGpuTimer[0] = m_graphicsDevice->createTimer();
                        if (createInfo->arraySize > 1) {
                            images.postProcessorGpuTimer[1] = m_graphicsDevice->createTimer();
                        }
                    }
                }

                m_swapchains.insert_or_assign(*swapchain, swapchainState);
            }

            return result;
        }

        XrResult xrDestroySwapchain(XrSwapchain swapchain) override {
            const XrResult result = OpenXrApi::xrDestroySwapchain(swapchain);
            if (XR_SUCCEEDED(result)) {
                m_swapchains.erase(swapchain);
            }

            return result;
        }

        XrResult xrSuggestInteractionProfileBindings(
            XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings) override {
            const XrResult result = OpenXrApi::xrSuggestInteractionProfileBindings(instance, suggestedBindings);
            if (XR_SUCCEEDED(result) && m_handTracker) {
                m_handTracker->registerBindings(*suggestedBindings);
            }

            return result;
        }

        XrResult xrCreateAction(XrActionSet actionSet,
                                const XrActionCreateInfo* createInfo,
                                XrAction* action) override {
            const XrResult result = OpenXrApi::xrCreateAction(actionSet, createInfo, action);
            if (XR_SUCCEEDED(result) && m_handTracker) {
                m_handTracker->registerAction(*action, actionSet);
            }

            return result;
        }

        XrResult xrDestroyAction(XrAction action) override {
            const XrResult result = OpenXrApi::xrDestroyAction(action);
            if (XR_SUCCEEDED(result) && m_handTracker) {
                m_handTracker->unregisterAction(action);
            }

            return result;
        }

        XrResult xrCreateActionSpace(XrSession session,
                                     const XrActionSpaceCreateInfo* createInfo,
                                     XrSpace* space) override {
            const XrResult result = OpenXrApi::xrCreateActionSpace(session, createInfo, space);
            if (XR_SUCCEEDED(result) && m_handTracker && isVrSession(session)) {
                // Keep track of the XrSpace for controllers, so we can override the behavior for them.
                const std::string fullPath = m_handTracker->getFullPath(createInfo->action, createInfo->subactionPath);
                if (fullPath == "/user/hand/right/input/grip/pose" || fullPath == "/user/hand/right/input/aim/pose" ||
                    fullPath == "/user/hand/left/input/grip/pose" || fullPath == "/user/hand/left/input/aim/pose") {
                    m_handTracker->registerActionSpace(*space, fullPath, createInfo->poseInActionSpace);
                }
            }

            return result;
        }

        XrResult xrDestroySpace(XrSpace space) override {
            const XrResult result = OpenXrApi::xrDestroySpace(space);
            if (XR_SUCCEEDED(result) && m_handTracker) {
                m_handTracker->unregisterActionSpace(space);
            }

            return result;
        }

        XrResult xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                            uint32_t imageCapacityInput,
                                            uint32_t* imageCountOutput,
                                            XrSwapchainImageBaseHeader* images) override {
            const XrResult result =
                OpenXrApi::xrEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
            if (XR_SUCCEEDED(result) && images) {
                auto swapchainIt = m_swapchains.find(swapchain);
                if (swapchainIt != m_swapchains.end()) {
                    auto swapchainState = swapchainIt->second;

                    // Return the application texture (first entry in the processing chain).
                    if (m_graphicsDevice->getApi() == graphics::Api::D3D11) {
                        XrSwapchainImageD3D11KHR* d3dImages = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
                        for (uint32_t i = 0; i < *imageCountOutput; i++) {
                            d3dImages[i].texture = swapchainState.images[i].chain[0]->getNative<graphics::D3D11>();
                        }
                    } else if (m_graphicsDevice->getApi() == graphics::Api::D3D12) {
                        XrSwapchainImageD3D12KHR* d3dImages = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
                        for (uint32_t i = 0; i < *imageCountOutput; i++) {
                            d3dImages[i].texture = swapchainState.images[i].chain[0]->getNative<graphics::D3D12>();
                        }
                    } else {
                        throw new std::runtime_error("Unsupported graphics runtime");
                    }
                }
            }

            return result;
        }

        XrResult xrAcquireSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageAcquireInfo* acquireInfo,
                                         uint32_t* index) override {
            const XrResult result = OpenXrApi::xrAcquireSwapchainImage(swapchain, acquireInfo, index);
            if (XR_SUCCEEDED(result)) {
                // Record the index so we know which texture to use in xrEndFrame().
                auto swapchainIt = m_swapchains.find(swapchain);
                if (swapchainIt != m_swapchains.end()) {
                    swapchainIt->second.acquiredImageIndex = *index;
                }
            }

            return result;
        }

        XrResult xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) override {
            if (m_sendInterationProfileEvent && m_vrSession != XR_NULL_HANDLE) {
                XrEventDataInteractionProfileChanged* const buffer =
                    reinterpret_cast<XrEventDataInteractionProfileChanged*>(eventData);
                buffer->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
                buffer->next = nullptr;
                buffer->session = m_vrSession;

                m_sendInterationProfileEvent = false;
                return XR_SUCCESS;
            }

            return OpenXrApi::xrPollEvent(instance, eventData);
        }

        XrResult xrGetCurrentInteractionProfile(XrSession session,
                                                XrPath topLevelUserPath,
                                                XrInteractionProfileState* interactionProfile) override {
            std::string path = topLevelUserPath != XR_NULL_PATH ? getPath(topLevelUserPath) : "";
            if (m_handTracker && isVrSession(session) &&
                (path.empty() || path == "/user/hand/left" || path == "/user/hand/right") &&
                interactionProfile->type == XR_TYPE_INTERACTION_PROFILE_STATE) {
                // Return our emulated interaction profile for the hands.
                interactionProfile->interactionProfile = m_handTracker->getInteractionProfile();
                return XR_SUCCESS;
            }

            return OpenXrApi::xrGetCurrentInteractionProfile(session, topLevelUserPath, interactionProfile);
        }

        XrResult xrLocateViews(XrSession session,
                               const XrViewLocateInfo* viewLocateInfo,
                               XrViewState* viewState,
                               uint32_t viewCapacityInput,
                               uint32_t* viewCountOutput,
                               XrView* views) override {
            const XrResult result =
                OpenXrApi::xrLocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
            if (XR_SUCCEEDED(result) && isVrSession(session) &&
                viewLocateInfo->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
                assert(*viewCountOutput == ViewCount);

                const auto vec = views[1].pose.position - views[0].pose.position;
                const auto ipd = Length(vec);

                // If it's the first time, initialize the ICD to be the same as IPD.
                int icdInTenthmm = m_configManager->getValue(config::SettingICD);
                if (icdInTenthmm == 0) {
                    icdInTenthmm = (int)(ipd * 10000.0f);
                    m_configManager->setValue(config::SettingICD, icdInTenthmm);
                }
                const float icd = icdInTenthmm / 10000.0f;

                // Override the ICD if requested. We can't do a real epsilon-compare since we use this weird tenth of mm
                // intermediate unit.
                if (std::abs(ipd - icd) > 0.00005f) {
                    const auto center = views[0].pose.position + vec / 2.0f;
                    const auto unit = Normalize(vec);

                    views[0].pose.position = center - unit * (icd / 2.0f);
                    views[1].pose.position = center + unit * (icd / 2.0f);
                }

                // Override the FOV if requested.
                const int fov = m_configManager->getValue(config::SettingFOV);
                if (fov != 100) {
                    const float multiplier = fov / 100.0f;

                    views[0].fov.angleUp *= multiplier;
                    views[0].fov.angleDown *= multiplier;
                    views[0].fov.angleLeft *= multiplier;
                    views[0].fov.angleRight *= multiplier;
                    views[1].fov.angleUp *= multiplier;
                    views[1].fov.angleDown *= multiplier;
                    views[1].fov.angleLeft *= multiplier;
                    views[1].fov.angleRight *= multiplier;
                }
            }

            return result;
        }

        XrResult xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) override {
            if (m_handTracker && location && m_handTracker->locate(space, baseSpace, time, *location)) {
                return XR_SUCCESS;
            }

            return OpenXrApi::xrLocateSpace(space, baseSpace, time, location);
        }

        XrResult xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) override {
            const XrResult result = OpenXrApi::xrSyncActions(session, syncInfo);
            if (XR_SUCCEEDED(result) && m_handTracker && isVrSession(session)) {
                m_handTracker->sync(m_begunFrameTime, *syncInfo);
            }

            return result;
        }

        XrResult xrGetActionStateBoolean(XrSession session,
                                         const XrActionStateGetInfo* getInfo,
                                         XrActionStateBoolean* state) override {
            if (m_handTracker && isVrSession(session) && getInfo->type == XR_TYPE_ACTION_STATE_GET_INFO &&
                state->type == XR_TYPE_ACTION_STATE_BOOLEAN) {
                if (m_handTracker->getActionState(*getInfo, *state)) {
                    return XR_SUCCESS;
                }
            }

            return OpenXrApi::xrGetActionStateBoolean(session, getInfo, state);
        }

        XrResult xrGetActionStateFloat(XrSession session,
                                       const XrActionStateGetInfo* getInfo,
                                       XrActionStateFloat* state) override {
            if (m_handTracker && isVrSession(session) && getInfo->type == XR_TYPE_ACTION_STATE_GET_INFO &&
                state->type == XR_TYPE_ACTION_STATE_FLOAT) {
                if (m_handTracker->getActionState(*getInfo, *state)) {
                    return XR_SUCCESS;
                }
            }

            return OpenXrApi::xrGetActionStateFloat(session, getInfo, state);
        }

        XrResult xrGetActionStatePose(XrSession session,
                                      const XrActionStateGetInfo* getInfo,
                                      XrActionStatePose* state) override {
            if (m_handTracker && isVrSession(session) && getInfo->type == XR_TYPE_ACTION_STATE_GET_INFO) {
                const std::string fullPath = m_handTracker->getFullPath(getInfo->action, getInfo->subactionPath);
                if ((fullPath == "/user/hand/right/input/grip/pose" || fullPath == "/user/hand/right/input/aim/pose" ||
                     fullPath == "/user/hand/left/input/grip/pose" || fullPath == "/user/hand/left/input/aim/pose") &&
                    state->type == XR_TYPE_ACTION_STATE_POSE) {
                    state->isActive = XR_TRUE;
                    return XR_SUCCESS;
                }
            }

            return OpenXrApi::xrGetActionStatePose(session, getInfo, state);
        }

        XrResult xrWaitFrame(XrSession session,
                             const XrFrameWaitInfo* frameWaitInfo,
                             XrFrameState* frameState) override {
            const XrResult result = OpenXrApi::xrWaitFrame(session, frameWaitInfo, frameState);
            if (XR_SUCCEEDED(result) && isVrSession(session)) {
                // Apply prediction dampening if possible and if needed.
                if (xrConvertWin32PerformanceCounterToTimeKHR) {
                    const int predictionDampen = m_configManager->getValue(config::SettingPredictionDampen);
                    if (predictionDampen != 100) {
                        // Find the current time.
                        LARGE_INTEGER qpcTimeNow;
                        QueryPerformanceCounter(&qpcTimeNow);

                        XrTime xrTimeNow;
                        CHECK_XRCMD(
                            xrConvertWin32PerformanceCounterToTimeKHR(GetXrInstance(), &qpcTimeNow, &xrTimeNow));

                        XrTime predictionAmount = frameState->predictedDisplayTime - xrTimeNow;
                        if (predictionAmount > 0) {
                            frameState->predictedDisplayTime = xrTimeNow + (predictionDampen * predictionAmount) / 100;
                        }

                        m_stats.predictionTimeUs += predictionAmount;
                    }
                }

                // Record the predicted display time.
                m_waitedFrameTime = frameState->predictedDisplayTime;
            }

            return result;
        }

        XrResult xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) override {
            const XrResult result = OpenXrApi::xrBeginFrame(session, frameBeginInfo);
            if (XR_SUCCEEDED(result) && isVrSession(session)) {
                // Record the predicted display time.
                m_begunFrameTime = m_waitedFrameTime;

                if (m_graphicsDevice) {
                    m_performanceCounters.appCpuTimer->start();
                    m_stats.appGpuTimeUs +=
                        m_performanceCounters.appGpuTimer[m_performanceCounters.gpuTimerIndex]->query();
                    m_performanceCounters.appGpuTimer[m_performanceCounters.gpuTimerIndex]->start();
                }
            }

            return result;
        }

        void updateStatisticsForFrame() {
            const auto now = std::chrono::steady_clock::now();
            const auto numFrames = ++m_performanceCounters.numFrames;

            if ((now - m_performanceCounters.lastWindowStart) >= std::chrono::seconds(1)) {
                m_performanceCounters.numFrames = 0;
                m_performanceCounters.lastWindowStart = now;

                // Push the last averaged statistics.
                m_stats.fps = static_cast<float>(numFrames);
                m_stats.appCpuTimeUs /= numFrames;
                m_stats.appGpuTimeUs /= numFrames;
                m_stats.endFrameCpuTimeUs /= numFrames;
                m_stats.upscalerGpuTimeUs /= numFrames;
                m_stats.preProcessorGpuTimeUs /= numFrames;
                m_stats.postProcessorGpuTimeUs /= numFrames;
                m_stats.overlayCpuTimeUs /= numFrames;
                m_stats.overlayGpuTimeUs /= numFrames;
                m_stats.predictionTimeUs /= numFrames;

                m_menuHandler->updateStatistics(m_stats);

                // Start from fresh!
                memset(&m_stats, 0, sizeof(m_stats));
            }
        }

        void updateConfiguration() {
            // Make sure config gets written if needed.
            m_configManager->tick();

            // Refresh the configuration.
            if (m_preProcessor) {
                m_preProcessor->update();
            }
            if (m_upscaler) {
                m_upscaler->update();
            }
            if (m_postProcessor) {
                m_postProcessor->update();
            }
        }

        void takeScreenshot(std::shared_ptr<graphics::ITexture> texture) const {
            std::stringstream parameters;
            if (m_upscaleMode != config::ScalingType::None) {
                // TODO: add a getUpscaleModeName() helper to keep enum and string in sync.
                const auto upscaleName = m_upscaleMode == config::ScalingType::NIS   ? "NIS_"
                                         : m_upscaleMode == config::ScalingType::FSR ? "FSR_"
                                                                                     : "SCL_";

                parameters << upscaleName << m_upscalingFactor << "_"
                           << m_configManager->getValue(config::SettingSharpness);
            }
            const std::time_t now = std::time(nullptr);
            char datetime[1024];
            std::strftime(datetime, sizeof(datetime), "%Y%m%d_%H%M%S_", std::localtime(&now));
            const std::string screenshotFilename = m_applicationName + "_" + datetime + parameters.str() + ".dds";
            std::string screenshotPath = (std::filesystem::path(getenv("LOCALAPPDATA")) / screenshotFilename).string();

            texture->saveToFile(screenshotPath);
        }

        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            if (!isVrSession(session) || !m_graphicsDevice) {
                return OpenXrApi::xrEndFrame(session, frameEndInfo);
            }

            updateStatisticsForFrame();

            m_performanceCounters.appCpuTimer->stop();
            m_stats.appCpuTimeUs += m_performanceCounters.appCpuTimer->query();
            m_performanceCounters.appGpuTimer[m_performanceCounters.gpuTimerIndex]->stop();

            m_stats.endFrameCpuTimeUs += m_performanceCounters.endFrameCpuTimer->query();
            m_performanceCounters.endFrameCpuTimer->start();

            // Toggle to the next set of GPU timers.
            m_performanceCounters.gpuTimerIndex = (m_performanceCounters.gpuTimerIndex + 1) % (GpuTimerLatency + 1);

            // Handle inputs.
            if (m_menuHandler) {
                m_menuHandler->handleInput();
            }

            // Prepare the Shaders for rendering.
            updateConfiguration();

            // Unbind all textures from the render targets.
            m_graphicsDevice->unsetRenderTargets();

            std::shared_ptr<graphics::ITexture> textureForOverlay[ViewCount] = {};
            XrCompositionLayerProjectionView* viewsForOverlay = nullptr;
            XrSpace spaceForOverlay = XR_NULL_HANDLE;

            // Because the frame info is passed const, we are going to need to reconstruct a writable version of it to
            // patch the resolution.
            XrFrameEndInfo chainFrameEndInfo = *frameEndInfo;
            std::vector<const XrCompositionLayerBaseHeader*> correctedLayers;

            std::vector<XrCompositionLayerProjection> layerProjectionAllocator;
            std::vector<std::array<XrCompositionLayerProjectionView, 2>> layerProjectionViewsAllocator;

            // We must reserve the underlying storage to keep our pointers stable.
            layerProjectionAllocator.reserve(chainFrameEndInfo.layerCount);
            layerProjectionViewsAllocator.reserve(chainFrameEndInfo.layerCount);

            // Apply the processing chain to all the (supported) layers.
            for (uint32_t i = 0; i < chainFrameEndInfo.layerCount; i++) {
                if (chainFrameEndInfo.layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const XrCompositionLayerProjection* proj =
                        reinterpret_cast<const XrCompositionLayerProjection*>(chainFrameEndInfo.layers[i]);

                    // To patch the resolution of the layer we need to recreate the whole projection & views
                    // data structures.
                    auto correctedProjectionLayer = &layerProjectionAllocator.emplace_back(*proj);
                    auto correctedProjectionViews = layerProjectionViewsAllocator
                                                        .emplace_back(std::array<XrCompositionLayerProjectionView, 2>(
                                                            {proj->views[0], proj->views[1]}))
                                                        .data();

                    // For VPRT, we need to handle texture arrays.
                    static_assert(ViewCount == 2);
                    const bool useVPRT = proj->views[0].subImage.swapchain == proj->views[1].subImage.swapchain;

                    assert(proj->viewCount == ViewCount);
                    for (uint32_t eye = 0; eye < ViewCount; eye++) {
                        const XrCompositionLayerProjectionView& view = proj->views[eye];

                        auto swapchainIt = m_swapchains.find(view.subImage.swapchain);
                        if (swapchainIt == m_swapchains.end()) {
                            throw new std::runtime_error("Swapchain is not registered");
                        }
                        auto swapchainState = swapchainIt->second;
                        auto swapchainImages = swapchainState.images[swapchainState.acquiredImageIndex];
                        uint32_t nextImage = 0;
                        uint32_t lastImage = 0;
                        uint32_t gpuTimerIndex = useVPRT ? eye : 0;

                        // TODO: Insert processing below.
                        // The pattern typically follows these steps:
                        // - Advanced to the right source and/or destination image;
                        // - Pull the previously measured timer value;
                        // - Start the timer;
                        // - Invoke the processing;
                        // - Stop the timer;
                        // - Advanced to the right source and/or destination image;

                        // Perform post-processing.
                        if (m_preProcessor) {
                            nextImage++;

                            m_stats.preProcessorGpuTimeUs +=
                                swapchainImages.preProcessorGpuTimer[gpuTimerIndex]->query();
                            swapchainImages.preProcessorGpuTimer[gpuTimerIndex]->start();

                            m_preProcessor->process(
                                swapchainImages.chain[lastImage], swapchainImages.chain[nextImage], useVPRT ? eye : -1);
                            swapchainImages.preProcessorGpuTimer[gpuTimerIndex]->stop();

                            lastImage++;
                        }

                        // Perform upscaling (if requested).
                        if (m_upscaler) {
                            nextImage++;

                            // We allow to bypass scaling when the menu option is turned off. This is only for quick
                            // comparison/testing, since we're still holding to all the underlying resources.
                            if (m_configManager->getEnumValue<config::ScalingType>(config::SettingScalingType) !=
                                config::ScalingType::None) {
                                m_stats.upscalerGpuTimeUs += swapchainImages.upscalerGpuTimer[gpuTimerIndex]->query();
                                swapchainImages.upscalerGpuTimer[gpuTimerIndex]->start();

                                m_upscaler->upscale(swapchainImages.chain[lastImage],
                                                    swapchainImages.chain[nextImage],
                                                    useVPRT ? eye : -1);
                                swapchainImages.upscalerGpuTimer[gpuTimerIndex]->stop();

                                lastImage++;
                            }
                        }

                        // Perform post-processing.
                        if (m_postProcessor) {
                            nextImage++;

                            m_stats.postProcessorGpuTimeUs +=
                                swapchainImages.postProcessorGpuTimer[gpuTimerIndex]->query();
                            swapchainImages.postProcessorGpuTimer[gpuTimerIndex]->start();

                            m_postProcessor->process(
                                swapchainImages.chain[lastImage], swapchainImages.chain[nextImage], useVPRT ? eye : -1);
                            swapchainImages.postProcessorGpuTimer[gpuTimerIndex]->stop();

                            lastImage++;
                        }

                        // Make sure the chain was completed.
                        if (nextImage != swapchainImages.chain.size() - 1) {
                            throw new std::runtime_error("Processing chain incomplete!");
                        }

                        textureForOverlay[eye] = swapchainImages.chain.back();

                        // Patch the resolution.
                        correctedProjectionViews[eye].subImage.imageRect.extent.width = m_displayWidth;
                        correctedProjectionViews[eye].subImage.imageRect.extent.height = m_displayHeight;

                        // Patch the FOV when set above 100%.
                        const int fov = m_configManager->getValue(config::SettingFOV);
                        if (fov > 100) {
                            const float multiplier = 100.0f / fov;

                            correctedProjectionViews[eye].fov.angleUp *= multiplier;
                            correctedProjectionViews[eye].fov.angleDown *= multiplier;
                            correctedProjectionViews[eye].fov.angleLeft *= multiplier;
                            correctedProjectionViews[eye].fov.angleRight *= multiplier;
                        }
                    }

                    viewsForOverlay = correctedProjectionViews;
                    spaceForOverlay = proj->space;

                    correctedProjectionLayer->views = correctedProjectionViews;
                    correctedLayers.push_back(
                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(correctedProjectionLayer));
                } else {
                    correctedLayers.push_back(chainFrameEndInfo.layers[i]);
                }
            }

            chainFrameEndInfo.layers = correctedLayers.data();

            // We intentionally exclude the overlay from this timer, as it has its own separate timer.
            m_performanceCounters.endFrameCpuTimer->stop();

            // Render our overlays.
            if (textureForOverlay[0]) {
                const bool useVPRT = textureForOverlay[1] == textureForOverlay[0];

                if (m_menuHandler || m_handTracker) {
                    m_stats.overlayCpuTimeUs += m_performanceCounters.overlayCpuTimer->query();
                    m_stats.overlayGpuTimeUs +=
                        m_performanceCounters.overlayGpuTimer[m_performanceCounters.gpuTimerIndex]->query();

                    m_performanceCounters.overlayCpuTimer->start();
                    m_performanceCounters.overlayGpuTimer[m_performanceCounters.gpuTimerIndex]->start();
                    m_graphicsDevice->saveContext();
                }

                if (m_menuHandler && m_needCalibrateEyeOffsets) {
                    m_menuHandler->calibrate(viewsForOverlay[0].pose,
                                             viewsForOverlay[0].fov,
                                             textureForOverlay[0]->getInfo(),
                                             viewsForOverlay[1].pose,
                                             viewsForOverlay[1].fov,
                                             textureForOverlay[1]->getInfo());
                    m_needCalibrateEyeOffsets = false;
                }

                // Render the hands.
                if (m_handTracker) {
                    for (uint32_t eye = 0; eye < ViewCount; eye++) {
                        if (!useVPRT) {
                            m_graphicsDevice->setRenderTargets({textureForOverlay[eye]});
                        } else {
                            m_graphicsDevice->setRenderTargets({std::make_pair(textureForOverlay[eye], eye)});
                        }
                        m_graphicsDevice->setViewProjection(
                            viewsForOverlay[eye].pose, viewsForOverlay[eye].fov, 0.001f, 100.0f);

                        m_handTracker->render(viewsForOverlay[eye].pose, spaceForOverlay, textureForOverlay[eye]);
                    }
                }

                // Render the menu.
                // Ideally, we would not have to split this from the branch above, however with D3D12 we are forced to
                // flush the context, and we'd rather do it only once.
                if (m_menuHandler) {
                    if (m_graphicsDevice->getApi() == graphics::Api::D3D12) {
                        m_graphicsDevice->flushContext();
                    }
                    for (uint32_t eye = 0; eye < ViewCount; eye++) {
                        if (!useVPRT) {
                            m_graphicsDevice->setRenderTargets({textureForOverlay[eye]});
                        } else {
                            m_graphicsDevice->setRenderTargets({std::make_pair(textureForOverlay[eye], eye)});
                        }

                        m_graphicsDevice->beginText();
                        m_menuHandler->render(eye, viewsForOverlay[eye].pose, textureForOverlay[eye]);
                        m_graphicsDevice->flushText();
                    }
                }

                if (m_menuHandler || m_handTracker) {
                    m_graphicsDevice->restoreContext();
                    m_performanceCounters.overlayCpuTimer->stop();
                    m_performanceCounters.overlayGpuTimer[m_performanceCounters.gpuTimerIndex]->stop();
                }
            }

            // Whether the menu is available or not, we can still use that top-most texture for screenshot.
            // TODO: The screenshot does not work with multi-layer applications.
            const bool requestScreenshot =
                utilities::UpdateKeyState(m_requestScreenShotKeyState, VK_CONTROL, VK_F12, false) &&
                m_configManager->getValue(config::SettingScreenshotEnabled);

            if (textureForOverlay[0] && requestScreenshot) {
                takeScreenshot(textureForOverlay[0]);
            }

            m_graphicsDevice->flushContext();

            return OpenXrApi::xrEndFrame(session, &chainFrameEndInfo);
        }

      private:
        bool isVrSystem(XrSystemId systemId) const {
            return systemId == m_vrSystemId;
        }

        bool isVrSession(XrSession session) const {
            return session == m_vrSession;
        }

        const std::string getPath(XrPath path) {
            char buf[XR_MAX_PATH_LENGTH];
            uint32_t count;
            CHECK_XRCMD(xrPathToString(GetXrInstance(), path, sizeof(buf), &count, buf));
            std::string str;
            str.assign(buf, count - 1);
            return str;
        }

        std::string m_applicationName;
        std::string m_runtimeName;
        XrSystemId m_vrSystemId{XR_NULL_SYSTEM_ID};
        XrSession m_vrSession{XR_NULL_HANDLE};
        uint32_t m_displayWidth{0};
        uint32_t m_displayHeight{0};
        bool m_supportHandTracking{false};

        XrTime m_waitedFrameTime;
        XrTime m_begunFrameTime;
        bool m_sendInterationProfileEvent{false};

        std::shared_ptr<config::IConfigManager> m_configManager;

        std::shared_ptr<graphics::IDevice> m_graphicsDevice;
        std::map<XrSwapchain, SwapchainState> m_swapchains;

        std::shared_ptr<graphics::IUpscaler> m_upscaler;
        config::ScalingType m_upscaleMode{config::ScalingType::None};
        uint32_t m_upscalingFactor{100};

        std::shared_ptr<graphics::IImageProcessor> m_preProcessor;
        std::shared_ptr<graphics::IImageProcessor> m_postProcessor;

        std::shared_ptr<input::IHandTracker> m_handTracker;

        std::shared_ptr<menu::IMenuHandler> m_menuHandler;
        bool m_requestScreenShotKeyState{false};
        bool m_needCalibrateEyeOffsets{true};

        struct {
            std::shared_ptr<utilities::ICpuTimer> appCpuTimer;
            std::shared_ptr<graphics::IGpuTimer> appGpuTimer[GpuTimerLatency + 1];
            std::shared_ptr<utilities::ICpuTimer> endFrameCpuTimer;
            std::shared_ptr<utilities::ICpuTimer> overlayCpuTimer;
            std::shared_ptr<graphics::IGpuTimer> overlayGpuTimer[GpuTimerLatency + 1];

            unsigned int gpuTimerIndex{0};
            std::chrono::steady_clock::time_point lastWindowStart;
            uint32_t numFrames{0};
        } m_performanceCounters;

        LayerStatistics m_stats{};

        // TODO: These should be auto-generated and accessible via OpenXrApi.
        PFN_xrConvertWin32PerformanceCounterToTimeKHR xrConvertWin32PerformanceCounterToTimeKHR{nullptr};
    };

    std::unique_ptr<OpenXrLayer> g_instance = nullptr;

} // namespace

namespace toolkit {
    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

    void ResetInstance() {
        g_instance.reset();
    }

} // namespace toolkit

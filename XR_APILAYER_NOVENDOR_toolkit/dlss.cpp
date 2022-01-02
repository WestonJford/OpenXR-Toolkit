// MIT License
//
// Copyright(c) 2022 Matthieu Bucchianeri
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

#include "pch.h"

#include "factories.h"
#include "interfaces.h"
#include "log.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#define CHECK_NVCMD(cmd) xr::detail::_CheckNVResult(cmd, #cmd, FILE_AND_LINE)

namespace xr::detail {

    [[noreturn]] inline void _ThrowNVResult(NVSDK_NGX_Result nvr,
                                            const char* originator = nullptr,
                                            const char* sourceLocation = nullptr) {
        xr::detail::_Throw(xr::detail::_Fmt("NVSDK_NGX_Result failure [%x]", nvr), originator, sourceLocation);
    }

    inline HRESULT _CheckNVResult(NVSDK_NGX_Result nvr,
                                  const char* originator = nullptr,
                                  const char* sourceLocation = nullptr) {
        if (NVSDK_NGX_FAILED(nvr)) {
            xr::detail::_ThrowNVResult(nvr, originator, sourceLocation);
        }

        return nvr;
    }

} // namespace xr::detail

namespace {

    using namespace toolkit;
    using namespace toolkit::config;
    using namespace toolkit::graphics;
    using namespace toolkit::log;

    class DLSSSuperSampler : public ISuperSampler {
      public:
        DLSSSuperSampler(std::shared_ptr<IConfigManager> configManager,
                         std::shared_ptr<IDevice> graphicsDevice,
                         uint32_t outputWidth,
                         uint32_t outputHeight)
            : m_configManager(configManager), m_device(graphicsDevice), m_outputWidth(outputWidth),
              m_outputHeight(outputHeight) {
            if (m_device->getApi() == Api::D3D11) {
                if (++numSuperSamplers == 1) {
                    std::string logHome = getenv("LOCALAPPDATA");

                    CHECK_NVCMD(NVSDK_NGX_D3D11_Init(
                        12345, std::wstring(logHome.begin(), logHome.end()).c_str(), m_device->getNative<D3D11>()));
                }
                CHECK_NVCMD(NVSDK_NGX_D3D11_GetCapabilityParameters(&m_ngxParameters));
            } else {
                throw new std::runtime_error("Unsupported graphics runtime");
            }
        }

        ~DLSSSuperSampler() override {
            if (m_device->getApi() == Api::D3D11) {
                if (m_dlssHandle) {
                    NVSDK_NGX_D3D11_ReleaseFeature(m_dlssHandle);
                }
                NVSDK_NGX_D3D11_DestroyParameters(m_ngxParameters);
                if (--numSuperSamplers == 0) {
                    NVSDK_NGX_D3D11_Shutdown();
                }
            }
        }

        void update() override {
        }

        void upscale(std::shared_ptr<ITexture> input,
                     std::shared_ptr<ITexture> motionVectors,
                     std::shared_ptr<ITexture> depth,
                     bool isDepthInverted,
                     std::shared_ptr<ITexture> output,
                     int32_t slice = -1) override {
            if (m_device->getApi() == Api::D3D11) {
                // Lazily-created DLSS handle (we need to know if depth is inverted!).
                if (!m_dlssHandle) {
                    NVSDK_NGX_DLSS_Create_Params createParams;
                    ZeroMemory(&createParams, sizeof(NVSDK_NGX_DLSS_Create_Params));
                    createParams.InFeatureCreateFlags = isDepthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted
                                                                        : NVSDK_NGX_DLSS_Feature_Flags_None;
                    createParams.Feature.InTargetWidth = m_outputWidth;
                    createParams.Feature.InTargetHeight = m_outputHeight;
                    createParams.Feature.InWidth = input->getInfo().width;
                    createParams.Feature.InHeight = input->getInfo().height;
                    // TODO: Should be a setting.
                    createParams.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced;
                    CHECK_NVCMD(NGX_D3D11_CREATE_DLSS_EXT(
                        m_device->getContext<D3D11>(), &m_dlssHandle, m_ngxParameters, &createParams));
                }

                // Invoke DLSS.
                NVSDK_NGX_D3D11_DLSS_Eval_Params dlssParams;
                ZeroMemory(&dlssParams, sizeof(NVSDK_NGX_D3D11_DLSS_Eval_Params));
                dlssParams.Feature.pInColor = input->getNative<D3D11>();
                dlssParams.Feature.pInOutput = output->getNative<D3D11>();
                dlssParams.Feature.InSharpness = m_configManager->getValue(SettingSharpness) / 100.0f;
                dlssParams.pInDepth = depth->getNative<D3D11>();
                dlssParams.pInMotionVectors = motionVectors->getNative<D3D11>();
                dlssParams.InJitterOffsetX = dlssParams.InJitterOffsetY = 0.0f;
                dlssParams.InRenderSubrectDimensions.Width = input->getInfo().width;
                dlssParams.InRenderSubrectDimensions.Height = input->getInfo().height;
                CHECK_NVCMD(NGX_D3D11_EVALUATE_DLSS_EXT(
                    m_device->getContext<D3D11>(), m_dlssHandle, m_ngxParameters, &dlssParams));
            } else {
                throw new std::runtime_error("Unsupported graphics runtime");
            }
        }

      private:
        const std::shared_ptr<IConfigManager> m_configManager;
        const std::shared_ptr<IDevice> m_device;
        const uint32_t m_outputWidth;
        const uint32_t m_outputHeight;

        NVSDK_NGX_Parameter* m_ngxParameters{nullptr};
        NVSDK_NGX_Handle* m_dlssHandle{nullptr};

        static inline std::atomic_uint numSuperSamplers{0};
    };

} // namespace

namespace toolkit::graphics {

    bool InitializeDLSSEngine(LUID adapterLuid) {
        // Create an ephemeral D3D11 device.
        ComPtr<ID3D11Device> d3d11Device;
        {
            ComPtr<IDXGIFactory1> dxgiFactory;
            CHECK_HRCMD(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)));

            for (UINT adapterIndex = 0;; adapterIndex++) {
                // EnumAdapters1 will fail with DXGI_ERROR_NOT_FOUND when there are no more adapters to enumerate.
                ComPtr<IDXGIAdapter1> dxgiAdapter;
                CHECK_HRCMD(dxgiFactory->EnumAdapters1(adapterIndex, &dxgiAdapter));

                DXGI_ADAPTER_DESC1 adapterDesc;
                CHECK_HRCMD(dxgiAdapter->GetDesc1(&adapterDesc));
                if (1 /* XXX */ || memcmp(&adapterDesc.AdapterLuid, &adapterLuid, sizeof(adapterLuid)) == 0) {
                    const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
                    CHECK_HRCMD(D3D11CreateDevice(dxgiAdapter.Get(),
                                                  D3D_DRIVER_TYPE_UNKNOWN,
                                                  0,
                                                  0,
                                                  &featureLevel,
                                                  1,
                                                  D3D11_SDK_VERSION,
                                                  &d3d11Device,
                                                  nullptr,
                                                  nullptr));
                    break;
                }
            }
        }

        int dlssAvailable = 0;
        try {
            // Create an ephemeral NGX instance.
            std::string logHome = getenv("LOCALAPPDATA");
            CHECK_NVCMD(
                NVSDK_NGX_D3D11_Init(12345, std::wstring(logHome.begin(), logHome.end()).c_str(), d3d11Device.Get()));

            NVSDK_NGX_Parameter* ngxParameters;
            CHECK_NVCMD(NVSDK_NGX_D3D11_GetCapabilityParameters(&ngxParameters));
            CHECK_NVCMD(ngxParameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlssAvailable));
            NVSDK_NGX_D3D11_DestroyParameters(ngxParameters);
            NVSDK_NGX_D3D11_Shutdown();
        } catch (std::exception exc) {
            Log("%s\n", exc.what());
        }
        return dlssAvailable;
    }

    // TODO: Query preferred resolution for given profile.

    std::shared_ptr<ISuperSampler> CreateDLSSSuperSampler(std::shared_ptr<IConfigManager> configManager,
                                                          std::shared_ptr<IDevice> graphicsDevice,
                                                          uint32_t outputWidth,
                                                          uint32_t outputHeight) {
        return std::make_shared<DLSSSuperSampler>(configManager, graphicsDevice, outputWidth, outputHeight);
    }

} // namespace toolkit::graphics

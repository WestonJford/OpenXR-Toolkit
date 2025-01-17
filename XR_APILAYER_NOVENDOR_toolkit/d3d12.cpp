// MIT License
//
// Copyright(c) 2021 Matthieu Bucchianeri
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

#include "d3dcommon.h"
#include "shader_utilities.h"
#include "factories.h"
#include "interfaces.h"
#include "log.h"

namespace {

    using namespace toolkit;
    using namespace toolkit::graphics;
    using namespace toolkit::graphics::d3dcommon;
    using namespace toolkit::log;

    struct D3D12Heap {
        void initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type) {
            D3D12_DESCRIPTOR_HEAP_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.NumDescriptors = 32;
            desc.Type = type;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER || type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
                desc.Flags |= D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            }
            CHECK_HRCMD(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)));
            heapStartCPU = heap->GetCPUDescriptorHandleForHeapStart();
            heapStartGPU = heap->GetGPUDescriptorHandleForHeapStart();
            heapOffset = 0;
            descSize = device->GetDescriptorHandleIncrementSize(type);
        }

        void allocate(D3D12_CPU_DESCRIPTOR_HANDLE& desc) {
            desc = CD3DX12_CPU_DESCRIPTOR_HANDLE(heapStartCPU, heapOffset++, descSize);
        }

        // TODO: Implement freeing a descriptor

        D3D12_GPU_DESCRIPTOR_HANDLE getGPUHandle(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) const {
            INT64 offset = (cpuHandle.ptr - heapStartCPU.ptr) / descSize;
            return CD3DX12_GPU_DESCRIPTOR_HANDLE(heapStartGPU, (INT)offset, descSize);
        }

        ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_CPU_DESCRIPTOR_HANDLE heapStartCPU;
        D3D12_GPU_DESCRIPTOR_HANDLE heapStartGPU;
        INT heapOffset;
        UINT descSize;
    };

    // Wrap shader resources, common code for root signature creation.
    // Upon first use of the shader, we require the use of the register*() method below to create the root signature.
    // When ready to invoke the shader for the first time, we ask the caller to "resolve" the root signature, which in
    // turn create the necessary pipeline state. This process assumes that the order of setInput/Output() calls
    // are going to be identical for a given shader, which is an acceptable constraint.
    class D3D12Shader {
      public:
        D3D12Shader(std::shared_ptr<IDevice> device, ID3DBlob* shaderBytes, const std::optional<std::string>& debugName)
            : m_device(device), m_shaderBytes(shaderBytes), m_debugName(debugName) {
        }

        virtual ~D3D12Shader() = default;

        void setOutputFormat(const XrSwapchainCreateInfo& info) {
            m_outputInfo = info;
        }

        void registerSamplerParameter(uint32_t slot, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
            m_parametersDescriptorRanges.push_back(
                CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, slot));
            m_parametersForFirstCall.push_back(std::make_pair((uint32_t)m_parametersForFirstCall.size(), handle));
        }

        void registerCBVParameter(uint32_t slot, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
            m_parametersDescriptorRanges.push_back(CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, slot));
            m_parametersForFirstCall.push_back(std::make_pair((uint32_t)m_parametersForFirstCall.size(), handle));
        }

        void registerSRVParameter(uint32_t slot, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
            m_parametersDescriptorRanges.push_back(CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, slot));
            m_parametersForFirstCall.push_back(std::make_pair((uint32_t)m_parametersForFirstCall.size(), handle));
        }

        void registerUAVParameter(uint32_t slot, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
            m_parametersDescriptorRanges.push_back(CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, slot));
            m_parametersForFirstCall.push_back(std::make_pair((uint32_t)m_parametersForFirstCall.size(), handle));
        }

        virtual void resolve() {
            // Common code for creating the root signature.
            auto device = m_device->getNative<D3D12>();

            std::vector<CD3DX12_ROOT_PARAMETER> parametersDescriptors;
            for (const auto& range : m_parametersDescriptorRanges) {
                parametersDescriptors.push_back({});
                parametersDescriptors.back().InitAsDescriptorTable(1, &range);
            }

            CD3DX12_ROOT_SIGNATURE_DESC desc((UINT)parametersDescriptors.size(),
                                             parametersDescriptors.data(),
                                             0,
                                             nullptr,
                                             D3D12_ROOT_SIGNATURE_FLAG_NONE);

            ComPtr<ID3DBlob> serializedRootSignature;
            ComPtr<ID3DBlob> errors;
            const HRESULT hr =
                D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSignature, &errors);
            if (FAILED(hr)) {
                if (errors) {
                    Log("%s", (char*)errors->GetBufferPointer());
                }
                CHECK_HRESULT(hr, "Failed to serialize root signature");
            }

            CHECK_HRCMD(device->CreateRootSignature(0,
                                                    serializedRootSignature->GetBufferPointer(),
                                                    serializedRootSignature->GetBufferSize(),
                                                    IID_PPV_ARGS(&m_rootSignature)));

            m_parametersDescriptorRanges.clear();
        }

        bool needsResolve() const {
            return !m_pipelineState;
        }

      protected:
        const std::shared_ptr<IDevice> m_device;
        // Keep a reference for memory management purposes.
        const ComPtr<ID3DBlob> m_shaderBytes;
        const std::optional<std::string> m_debugName;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_pipelineState;

        // Only used during pre-resolve phase.
        XrSwapchainCreateInfo m_outputInfo{};
        std::vector<CD3DX12_DESCRIPTOR_RANGE> m_parametersDescriptorRanges;
        std::vector<std::pair<uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE>> m_parametersForFirstCall;

        mutable struct D3D12::ShaderData m_shaderData;
    };

    class D3D12QuadShader : public D3D12Shader, public IQuadShader {
      public:
        D3D12QuadShader(std::shared_ptr<IDevice> device,
                        D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                        ID3DBlob* shaderBytes,
                        const std::optional<std::string>& debugName)
            : D3D12Shader(device, shaderBytes, debugName), m_psoDesc(desc) {
        }

        Api getApi() const override {
            return Api::D3D12;
        }

        std::shared_ptr<IDevice> getDevice() const override {
            return m_device;
        }

        void resolve() override {
            // Create the root signature now.
            D3D12Shader::resolve();

            // Initialize the pipeline state now.
            auto device = m_device->getNative<D3D12>();
            m_psoDesc.RTVFormats[0] = (DXGI_FORMAT)m_outputInfo.format;
            m_psoDesc.NumRenderTargets = 1;
            m_psoDesc.SampleDesc.Count = m_outputInfo.sampleCount;
            if (m_psoDesc.SampleDesc.Count > 1) {
                D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qualityLevels;
                qualityLevels.Format = m_psoDesc.RTVFormats[0];
                qualityLevels.SampleCount = m_psoDesc.SampleDesc.Count;
                qualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
                CHECK_HRCMD(m_device->getNative<D3D12>()->CheckFeatureSupport(
                    D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qualityLevels, sizeof(qualityLevels)));

                // Setup for highest quality multisampling if requested.
                m_psoDesc.SampleDesc.Quality = qualityLevels.NumQualityLevels - 1;
                m_psoDesc.RasterizerState.MultisampleEnable = true;
            }
            m_psoDesc.pRootSignature = m_rootSignature.Get();
            CHECK_HRCMD(device->CreateGraphicsPipelineState(&m_psoDesc, IID_PPV_ARGS(&m_pipelineState)));

            if (m_debugName) {
                m_pipelineState->SetName(std::wstring(m_debugName->begin(), m_debugName->end()).c_str());
            }

            m_shaderData.rootSignature = m_rootSignature.Get();
            m_shaderData.pipelineState = m_pipelineState.Get();

            // Setup the pipeline to make up for the deferred initialization.
            auto context = m_device->getContext<D3D12>();
            context->SetGraphicsRootSignature(m_rootSignature.Get());
            context->SetPipelineState(m_pipelineState.Get());
            context->IASetIndexBuffer(nullptr);
            context->IASetVertexBuffers(0, 0, nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            for (const auto& parameter : m_parametersForFirstCall) {
                context->SetGraphicsRootDescriptorTable(parameter.first, parameter.second);
            }

            m_parametersForFirstCall.clear();
        }

        void* getNativePtr() const override {
            return reinterpret_cast<void*>(&m_shaderData);
        }

      private:
        D3D12_GRAPHICS_PIPELINE_STATE_DESC m_psoDesc;
    };

    class D3D12ComputeShader : public D3D12Shader, public IComputeShader {
      public:
        D3D12ComputeShader(std::shared_ptr<IDevice> device,
                           D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
                           ID3DBlob* shaderBytes,
                           const std::optional<std::string>& debugName,
                           std::optional<std::array<unsigned int, 3>> threadGroups)
            : D3D12Shader(device, shaderBytes, debugName), m_psoDesc(desc) {
            if (threadGroups) {
                m_threadGroups = threadGroups.value();
            }
        }

        Api getApi() const override {
            return Api::D3D12;
        }

        std::shared_ptr<IDevice> getDevice() const override {
            return m_device;
        }

        void updateThreadGroups(const std::array<unsigned int, 3>& threadGroups) override {
            m_threadGroups = threadGroups;
        }

        const std::array<unsigned int, 3>& getThreadGroups() const override {
            return m_threadGroups;
        }

        void resolve() override {
            // Create the root signature now.
            D3D12Shader::resolve();

            // Initialize the pipeline state now.
            auto device = m_device->getNative<D3D12>();
            m_psoDesc.pRootSignature = m_rootSignature.Get();
            CHECK_HRCMD(device->CreateComputePipelineState(&m_psoDesc, IID_PPV_ARGS(&m_pipelineState)));

            if (m_debugName) {
                m_pipelineState->SetName(std::wstring(m_debugName->begin(), m_debugName->end()).c_str());
            }

            m_shaderData.rootSignature = m_rootSignature.Get();
            m_shaderData.pipelineState = m_pipelineState.Get();

            // Setup the pipeline to make up for the deferred initialization.
            auto context = m_device->getContext<D3D12>();
            context->SetComputeRootSignature(m_rootSignature.Get());
            context->SetPipelineState(m_pipelineState.Get());
            for (const auto& parameter : m_parametersForFirstCall) {
                context->SetComputeRootDescriptorTable(parameter.first, parameter.second);
            }

            m_parametersForFirstCall.clear();
        }

        void* getNativePtr() const override {
            return reinterpret_cast<void*>(&m_shaderData);
        }

      private:
        std::array<unsigned int, 3> m_threadGroups;

        D3D12_COMPUTE_PIPELINE_STATE_DESC m_psoDesc;
    };

    // Wrap a resource view. Obtained from D3D12Texture.
    class D3D12ResourceView : public IShaderInputTextureView,
                              public IComputeShaderOutputView,
                              public IRenderTargetView,
                              public IDepthStencilView {
      public:
        D3D12ResourceView(std::shared_ptr<IDevice> device, D3D12_CPU_DESCRIPTOR_HANDLE resourceView)
            : m_device(device), m_resourceView(resourceView) {
        }

        Api getApi() const override {
            return Api::D3D12;
        }

        std::shared_ptr<IDevice> getDevice() const override {
            return m_device;
        }

        void* getNativePtr() const override {
            return reinterpret_cast<void*>(const_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(&m_resourceView));
        }

      private:
        const std::shared_ptr<IDevice> m_device;
        const D3D12_CPU_DESCRIPTOR_HANDLE m_resourceView;
    };

    // Wrap a texture resource. Obtained from D3D12Device.
    class D3D12Texture : public ITexture {
      public:
        D3D12Texture(std::shared_ptr<IDevice> device,
                     const XrSwapchainCreateInfo& info,
                     const D3D12_RESOURCE_DESC& textureDesc,
                     ID3D12Resource* texture,
                     D3D12Heap& rtvHeap,
                     D3D12Heap& dsvHeap,
                     D3D12Heap& rvHeap)
            : m_device(device), m_info(info), m_textureDesc(textureDesc), m_texture(texture), m_rtvHeap(rtvHeap),
              m_dsvHeap(dsvHeap), m_rvHeap(rvHeap) {
            m_shaderResourceSubView.resize(info.arraySize);
            m_unorderedAccessSubView.resize(info.arraySize);
            m_renderTargetSubView.resize(info.arraySize);
        }

        Api getApi() const override {
            return Api::D3D12;
        }

        std::shared_ptr<IDevice> getDevice() const override {
            return m_device;
        }

        const XrSwapchainCreateInfo& getInfo() const override {
            return m_info;
        }

        bool isArray() const override {
            return m_textureDesc.DepthOrArraySize > 1;
        }

        std::shared_ptr<IShaderInputTextureView> getShaderInputView() const override {
            return getShaderInputViewInternal(m_shaderResourceView, 0);
        }

        std::shared_ptr<IShaderInputTextureView> getShaderInputView(uint32_t slice) const override {
            return getShaderInputViewInternal(m_shaderResourceSubView[slice], slice);
        }

        std::shared_ptr<IComputeShaderOutputView> getComputeShaderOutputView() const override {
            return getComputeShaderOutputViewInternal(m_unorderedAccessView, 0);
        }

        std::shared_ptr<IComputeShaderOutputView> getComputeShaderOutputView(uint32_t slice) const override {
            return getComputeShaderOutputViewInternal(m_unorderedAccessSubView[slice], slice);
        }

        std::shared_ptr<IRenderTargetView> getRenderTargetView() const override {
            return getRenderTargetViewInternal(m_renderTargetView, 0);
        }

        std::shared_ptr<IRenderTargetView> getRenderTargetView(uint32_t slice) const override {
            return getRenderTargetViewInternal(m_renderTargetSubView[slice], slice);
        }

        std::shared_ptr<IDepthStencilView> getDepthStencilView() const override {
            return getDepthStencilViewInternal(m_depthStencilView, 0);
        }

        std::shared_ptr<IDepthStencilView> getDepthStencilView(uint32_t slice) const override {
            return getDepthStencilViewInternal(m_depthStencilSubView[slice], slice);
        }

        void saveToFile(const std::string& path) const override {
            // TODO: Implement this.
        }

        void* getNativePtr() const override {
            return m_texture.Get();
        }

      private:
        std::shared_ptr<D3D12ResourceView>
        getShaderInputViewInternal(std::shared_ptr<D3D12ResourceView>& shaderResourceView, uint32_t slice = 0) const {
            if (!shaderResourceView) {
                if (m_textureDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) {
                    throw new std::runtime_error("Texture was created with D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE");
                }

                auto device = m_device->getNative<D3D12>();

                D3D12_SHADER_RESOURCE_VIEW_DESC desc;
                ZeroMemory(&desc, sizeof(desc));
                desc.Format = (DXGI_FORMAT)m_info.format;
                desc.ViewDimension =
                    m_info.arraySize == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                desc.Texture2DArray.ArraySize = 1;
                desc.Texture2DArray.FirstArraySlice = slice;
                desc.Texture2DArray.MipLevels = m_info.mipCount;
                desc.Texture2DArray.MostDetailedMip = D3D12CalcSubresource(0, 0, 0, m_info.mipCount, m_info.arraySize);

                D3D12_CPU_DESCRIPTOR_HANDLE handle;
                m_rvHeap.allocate(handle);
                device->CreateShaderResourceView(m_texture.Get(), &desc, handle);
                shaderResourceView = std::make_shared<D3D12ResourceView>(m_device, handle);
            }
            return shaderResourceView;
        }

        std::shared_ptr<D3D12ResourceView> getComputeShaderOutputViewInternal(
            std::shared_ptr<D3D12ResourceView>& unorderedAccessView, uint32_t slice = 0) const {
            if (!unorderedAccessView) {
                if (!(m_textureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
                    throw new std::runtime_error(
                        "Texture was not created with D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS");
                }

                auto device = m_device->getNative<D3D12>();

                D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
                ZeroMemory(&desc, sizeof(desc));
                desc.Format = (DXGI_FORMAT)m_info.format;
                desc.ViewDimension =
                    m_info.arraySize == 1 ? D3D12_UAV_DIMENSION_TEXTURE2D : D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.ArraySize = 1;
                desc.Texture2DArray.FirstArraySlice = slice;
                desc.Texture2DArray.MipSlice = D3D12CalcSubresource(0, 0, 0, m_info.mipCount, m_info.arraySize);

                D3D12_CPU_DESCRIPTOR_HANDLE handle;
                m_rvHeap.allocate(handle);
                device->CreateUnorderedAccessView(m_texture.Get(), nullptr, &desc, handle);
                unorderedAccessView = std::make_shared<D3D12ResourceView>(m_device, handle);
            }
            return unorderedAccessView;
        }

        std::shared_ptr<D3D12ResourceView>
        getRenderTargetViewInternal(std::shared_ptr<D3D12ResourceView>& renderTargetView, uint32_t slice = 0) const {
            if (!renderTargetView) {
                if (!(m_textureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
                    throw new std::runtime_error(
                        "Texture was not created with D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET");
                }

                auto device = m_device->getNative<D3D12>();

                D3D12_RENDER_TARGET_VIEW_DESC desc;
                ZeroMemory(&desc, sizeof(desc));
                desc.Format = (DXGI_FORMAT)m_info.format;
                desc.ViewDimension =
                    m_info.arraySize == 1 ? D3D12_RTV_DIMENSION_TEXTURE2D : D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.ArraySize = 1;
                desc.Texture2DArray.FirstArraySlice = slice;
                desc.Texture2DArray.MipSlice = D3D12CalcSubresource(0, 0, 0, m_info.mipCount, m_info.arraySize);

                D3D12_CPU_DESCRIPTOR_HANDLE handle;
                m_rtvHeap.allocate(handle);
                device->CreateRenderTargetView(m_texture.Get(), &desc, handle);
                renderTargetView = std::make_shared<D3D12ResourceView>(m_device, handle);
            }
            return renderTargetView;
        }

        std::shared_ptr<D3D12ResourceView>
        getDepthStencilViewInternal(std::shared_ptr<D3D12ResourceView>& depthStencilView, uint32_t slice = 0) const {
            if (!depthStencilView) {
                if (!(m_textureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
                    throw new std::runtime_error(
                        "Texture was not created with D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL");
                }

                auto device = m_device->getNative<D3D12>();

                D3D12_DEPTH_STENCIL_VIEW_DESC desc;
                ZeroMemory(&desc, sizeof(desc));
                desc.Format = (DXGI_FORMAT)m_info.format;
                desc.ViewDimension =
                    m_info.arraySize == 1 ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.ArraySize = 1;
                desc.Texture2DArray.FirstArraySlice = slice;
                desc.Texture2DArray.MipSlice = D3D12CalcSubresource(0, 0, 0, m_info.mipCount, m_info.arraySize);

                D3D12_CPU_DESCRIPTOR_HANDLE handle;
                m_dsvHeap.allocate(handle);
                device->CreateDepthStencilView(m_texture.Get(), &desc, handle);
                depthStencilView = std::make_shared<D3D12ResourceView>(m_device, handle);
            }
            return depthStencilView;
        }

        void setInteropTexture(std::shared_ptr<ITexture> interopTexture) {
            m_interopTexture = interopTexture;
        }

        std::shared_ptr<ITexture> getInteropTexture() {
            return m_interopTexture;
        }

        const std::shared_ptr<IDevice> m_device;
        const XrSwapchainCreateInfo m_info;
        const D3D12_RESOURCE_DESC m_textureDesc;
        const ComPtr<ID3D12Resource> m_texture;

        std::shared_ptr<ITexture> m_interopTexture;

        D3D12Heap& m_rtvHeap;
        D3D12Heap& m_dsvHeap;
        D3D12Heap& m_rvHeap;

        mutable std::shared_ptr<D3D12ResourceView> m_shaderResourceView;
        mutable std::vector<std::shared_ptr<D3D12ResourceView>> m_shaderResourceSubView;
        mutable std::shared_ptr<D3D12ResourceView> m_unorderedAccessView;
        mutable std::vector<std::shared_ptr<D3D12ResourceView>> m_unorderedAccessSubView;
        mutable std::shared_ptr<D3D12ResourceView> m_renderTargetView;
        mutable std::vector<std::shared_ptr<D3D12ResourceView>> m_renderTargetSubView;
        mutable std::shared_ptr<D3D12ResourceView> m_depthStencilView;
        mutable std::vector<std::shared_ptr<D3D12ResourceView>> m_depthStencilSubView;

        friend class D3D12Device;
    };

    class D3D12Buffer : public IShaderBuffer {
      public:
        D3D12Buffer(std::shared_ptr<IDevice> device,
                    D3D12_RESOURCE_DESC bufferDesc,
                    ID3D12Resource* buffer,
                    D3D12Heap& rvHeap,
                    ID3D12Resource* uploadBuffer = nullptr)
            : m_device(device), m_bufferDesc(bufferDesc), m_buffer(buffer), m_rvHeap(rvHeap),
              m_uploadBuffer(uploadBuffer) {
        }

        Api getApi() const override {
            return Api::D3D12;
        }

        std::shared_ptr<IDevice> getDevice() const override {
            return m_device;
        }

        void uploadData(const void* buffer, size_t count, ID3D12Resource* uploadBuffer) {
            D3D12_SUBRESOURCE_DATA subresourceData;
            ZeroMemory(&subresourceData, sizeof(subresourceData));
            subresourceData.pData = buffer;
            subresourceData.RowPitch = subresourceData.SlicePitch = count;

            auto context = m_device->getContext<D3D12>();

            {
                const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    m_buffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                context->ResourceBarrier(1, &barrier);
            }
            UpdateSubresources<1>(context, m_buffer.Get(), uploadBuffer, 0, 0, 1, &subresourceData);
            {
                const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    m_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
                context->ResourceBarrier(1, &barrier);
            }
        }

        void uploadData(const void* buffer, size_t count) override {
            if (!m_uploadBuffer) {
                throw new std::runtime_error("Texture is immutable");
            }
            uploadData(buffer, count, m_uploadBuffer.Get());
        }

        // TODO: Consider moving this operation up to IShaderBuffer. Will prevent the need for dynamic_cast below.
        D3D12_CPU_DESCRIPTOR_HANDLE getConstantBufferView() const {
            if (!m_constantBufferView) {
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE handle;
                    m_rvHeap.allocate(handle);
                    m_constantBufferView = handle;
                }

                auto device = m_device->getNative<D3D12>();

                D3D12_CONSTANT_BUFFER_VIEW_DESC desc;
                desc.BufferLocation = m_buffer->GetGPUVirtualAddress();
                desc.SizeInBytes = Align(m_bufferDesc.Width, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
                device->CreateConstantBufferView(&desc, m_constantBufferView.value());
            }
            return m_constantBufferView.value();
        }

        void* getNativePtr() const override {
            return m_buffer.Get();
        }

      private:
        const std::shared_ptr<IDevice> m_device;
        const D3D12_RESOURCE_DESC m_bufferDesc;
        const ComPtr<ID3D12Resource> m_buffer;

        D3D12Heap& m_rvHeap;

        const ComPtr<ID3D12Resource> m_uploadBuffer;

        mutable std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> m_constantBufferView;
    };

    // Wrap a vertex+indices buffers. Obtained from D3D12Device.
    class D3D12SimpleMesh : public ISimpleMesh {
      public:
        D3D12SimpleMesh(std::shared_ptr<IDevice> device,
                        ID3D12Resource* vertexBuffer,
                        size_t stride,
                        ID3D12Resource* indexBuffer,
                        size_t numIndices)
            : m_device(device), m_vertexBuffer(vertexBuffer), m_indexBuffer(indexBuffer) {
            m_meshData.vertexBuffer = m_vertexBuffer.Get();
            m_meshData.stride = (UINT)stride;
            m_meshData.indexBuffer = m_indexBuffer.Get();
            m_meshData.numIndices = (UINT)numIndices;
        }

        Api getApi() const override {
            return Api::D3D12;
        }

        std::shared_ptr<IDevice> getDevice() const override {
            return m_device;
        }

        void* getNativePtr() const override {
            return reinterpret_cast<void*>(&m_meshData);
        }

      private:
        const std::shared_ptr<IDevice> m_device;
        const ComPtr<ID3D12Resource> m_vertexBuffer;
        const ComPtr<ID3D12Resource> m_indexBuffer;

        mutable struct D3D12::MeshData m_meshData;
    };

    class D3D12GpuTimer : public IGpuTimer {
      public:
        D3D12GpuTimer(std::shared_ptr<IDevice> device) : m_device(device) {
        }

        Api getApi() const override {
            return Api::D3D12;
        }

        std::shared_ptr<IDevice> getDevice() const override {
            return m_device;
        }

        void start() override {
            // TODO: Implement this.
        }

        void stop() override {
            // TODO: Implement this.
        }

        uint64_t query(bool reset) const override {
            // TODO: Implement this.
            return 0;
        }

      private:
        const std::shared_ptr<IDevice> m_device;
    };

    class D3D12Device : public IDevice, public std::enable_shared_from_this<D3D12Device> {
      private:
        // OpenXR will not allow more than 2 frames in-flight, so 2 would be sufficient, however we might split the
        // processing in two due to text rendering, so multiply this number by 2.
        static constexpr size_t NumInflightContexts = 4;

      public:
        D3D12Device(ID3D12Device* device, ID3D12CommandQueue* queue) : m_device(device), m_queue(queue) {
            {
                ComPtr<IDXGIFactory1> dxgiFactory;
                CHECK_HRCMD(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)));
                const LUID adapterLuid = m_device->GetAdapterLuid();

                for (UINT adapterIndex = 0;; adapterIndex++) {
                    // EnumAdapters1 will fail with DXGI_ERROR_NOT_FOUND when there are no more adapters to enumerate.
                    ComPtr<IDXGIAdapter1> dxgiAdapter;
                    CHECK_HRCMD(dxgiFactory->EnumAdapters1(adapterIndex, &dxgiAdapter));

                    DXGI_ADAPTER_DESC1 adapterDesc;
                    CHECK_HRCMD(dxgiAdapter->GetDesc1(&adapterDesc));
                    if (!memcmp(&adapterDesc.AdapterLuid, &adapterLuid, sizeof(adapterLuid))) {
                        const std::wstring wadapterDescription(adapterDesc.Description);
                        std::transform(wadapterDescription.begin(),
                                       wadapterDescription.end(),
                                       std::back_inserter(m_deviceName),
                                       [](wchar_t c) { return (char)c; });

                        // Log the adapter name to help debugging customer issues.
                        Log("Using Direct3D 12 on adapter: %s\n", m_deviceName.c_str());
                        break;
                    }
                }
            }

            // Initialize the command lists and heaps.
            m_rtvHeap.initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            m_dsvHeap.initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            m_rvHeap.initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            m_samplerHeap.initialize(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            {
                D3D12_QUERY_HEAP_DESC desc;
                ZeroMemory(&desc, sizeof(desc));
                desc.Count = 16;
                desc.NodeMask = 1;
                desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                m_device->CreateQueryHeap(&desc, IID_PPV_ARGS(&m_queryHeap));
                m_queryHeap->SetName(L"Timestamp Query Heap");

                uint64_t gpuFrequency;
                m_queue->GetTimestampFrequency(&gpuFrequency);
                m_gpuTickDelta = 1.0 / gpuFrequency;
            }
            {
                for (uint32_t i = 0; i < NumInflightContexts; i++) {
                    CHECK_HRCMD(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                 IID_PPV_ARGS(&m_commandAllocator[i])));
                    CHECK_HRCMD(m_device->CreateCommandList(0,
                                                            D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            m_commandAllocator[i].Get(),
                                                            nullptr,
                                                            IID_PPV_ARGS(&m_commandList[i])));

                    // Set to a known state.
                    if (i != 0) {
                        CHECK_HRCMD(m_commandList[i]->Close());
                    }
                }
                m_currentContext = 0;
                m_context = m_commandList[0];
            }

            // Initialize the D3D11on12 interop device that we need for text rendering.
            // We use the text rendering primitives from the D3D11Device implmenentation (d3d11.cpp).
            {
                ComPtr<ID3D11Device> textDevice;
                D3D_FEATURE_LEVEL featureLevel = {D3D_FEATURE_LEVEL_11_1};
                CHECK_HRCMD(D3D11On12CreateDevice(device,
                                                  D3D11_CREATE_DEVICE_SINGLETHREADED,
                                                  &featureLevel,
                                                  1,
                                                  reinterpret_cast<IUnknown**>(&queue),
                                                  1,
                                                  0,
                                                  &textDevice,
                                                  nullptr,
                                                  nullptr));
                CHECK_HRCMD(textDevice->QueryInterface(__uuidof(ID3D11On12Device),
                                                       reinterpret_cast<void**>(m_textInteropDevice.GetAddressOf())));

                m_textDevice = WrapD3D11TextDevice(textDevice.Get());
            }

            CHECK_HRCMD(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));

            initializeShadingResources();
        }

        ~D3D12Device() override {
            Log("D3D12Device destroyed\n");
        }

        void shutdown() override {
            // Clear all references that could hold a cyclic reference themselves.
            m_currentComputeShader.reset();
            m_currentQuadShader.reset();
            m_currentDrawRenderTarget.reset();
            m_currentDrawDepthBuffer.reset();
            m_currentTextRenderTarget.reset();
        }

        Api getApi() const override {
            return Api::D3D12;
        }

        const std::string& getDeviceName() const override {
            return m_deviceName;
        }

        int64_t getTextureFormat(TextureFormat format) const override {
            switch (format) {
            case TextureFormat::R32G32B32A32_FLOAT:
                return (int64_t)DXGI_FORMAT_R32G32B32A32_FLOAT;

            case TextureFormat::R16G16B16A16_UNORM:
                return (int64_t)DXGI_FORMAT_R16G16B16A16_UNORM;

            case TextureFormat::R10G10B10A2_UNORM:
                return (int64_t)DXGI_FORMAT_R10G10B10A2_UNORM;

            case TextureFormat::R8G8B8A8_UNORM:
                return (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM;

            default:
                throw new std::runtime_error("Unknown texture format");
            };
        }

        bool isTextureFormatSRGB(int64_t format) const override {
            switch ((DXGI_FORMAT)format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                return true;
            default:
                return false;
            };
        }

        void saveContext(bool clear) override {
            // We make the assumption that the context saving/restoring is only used once per xrEndFrame() to avoid
            // trashing the application state. In D3D12, there is no such issue since the command list is separate from
            // the command queue.
        }

        void restoreContext() override {
            // We make the assumption that the context saving/restoring is only used once per xrEndFrame() to avoid
            // trashing the application state. In D3D12, there is no such issue since the command list is separate from
            // the command queue.
        }

        void flushContext(bool blocking) override {
            CHECK_HRCMD(m_context->Close());

            ID3D12CommandList* lists[] = {m_context.Get()};
            m_queue->ExecuteCommandLists(1, lists);

            if (blocking) {
                m_queue->Signal(m_fence.Get(), ++m_fenceValue);
                if (m_fence->GetCompletedValue() < m_fenceValue) {
                    HANDLE eventHandle = CreateEventEx(nullptr, L"flushContext Fence", 0, EVENT_ALL_ACCESS);
                    CHECK_HRCMD(m_fence->SetEventOnCompletion(m_fenceValue, eventHandle));
                    WaitForSingleObject(eventHandle, INFINITE);
                    CloseHandle(eventHandle);
                }
            }

            m_currentContext++;
            if (m_currentContext == NumInflightContexts) {
                m_currentContext = 0;
            }
            CHECK_HRCMD(m_commandAllocator[m_currentContext]->Reset());
            CHECK_HRCMD(m_commandList[m_currentContext]->Reset(m_commandAllocator[m_currentContext].Get(), nullptr));
            m_context = m_commandList[m_currentContext];
        }

        std::shared_ptr<ITexture> createTexture(const XrSwapchainCreateInfo& info,
                                                const std::optional<std::string>& debugName,
                                                uint32_t rowPitch = 0,
                                                uint32_t imageSize = 0,
                                                const void* initialData = nullptr) override {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                (DXGI_FORMAT)info.format, info.width, info.height, info.arraySize, info.mipCount, info.sampleCount);

            D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
            if (info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) {
                desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            if (info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            if (!(info.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT)) {
                desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
            }
            if (info.usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) {
                desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }

            ComPtr<ID3D12Resource> texture;
            const auto& heapType = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            CHECK_HRCMD(m_device->CreateCommittedResource(
                &heapType, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&texture)));

            if (initialData) {
                // Create an upload buffer.
                ComPtr<ID3D12Resource> uploadBuffer;
                {
                    const auto& heapType = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
                    const auto stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(imageSize);
                    CHECK_HRCMD(m_device->CreateCommittedResource(&heapType,
                                                                  D3D12_HEAP_FLAG_NONE,
                                                                  &stagingDesc,
                                                                  D3D12_RESOURCE_STATE_GENERIC_READ,
                                                                  nullptr,
                                                                  IID_PPV_ARGS(&uploadBuffer)));
                }
                {
                    void* mappedBuffer = nullptr;
                    uploadBuffer->Map(0, nullptr, &mappedBuffer);
                    memcpy(mappedBuffer, initialData, imageSize);
                    uploadBuffer->Unmap(0, nullptr);
                }

                // Do the upload now.
                {
                    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                        texture.Get(), initialState, D3D12_RESOURCE_STATE_COPY_DEST);
                    m_context->ResourceBarrier(1, &barrier);
                }
                {
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
                    ZeroMemory(&footprint, sizeof(footprint));
                    footprint.Footprint.Width = (UINT)desc.Width;
                    footprint.Footprint.Height = desc.Height;
                    footprint.Footprint.Depth = 1;
                    footprint.Footprint.RowPitch = Align(rowPitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
                    footprint.Footprint.Format = desc.Format;
                    CD3DX12_TEXTURE_COPY_LOCATION src(uploadBuffer.Get(), footprint);
                    CD3DX12_TEXTURE_COPY_LOCATION dst(texture.Get(), 0);
                    m_context->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
                {
                    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                        texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, initialState);
                    m_context->ResourceBarrier(1, &barrier);
                }
                flushContext(true);
            }

            if (debugName) {
                texture->SetName(std::wstring(debugName->begin(), debugName->end()).c_str());
            }

            return std::make_shared<D3D12Texture>(
                shared_from_this(), info, desc, texture.Get(), m_rtvHeap, m_dsvHeap, m_rvHeap);
        }

        std::shared_ptr<IShaderBuffer> createBuffer(size_t size,
                                                    const std::optional<std::string>& debugName,
                                                    const void* initialData,
                                                    bool immutable) override {
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);

            ComPtr<ID3D12Resource> buffer;
            {
                const auto& heapType = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                CHECK_HRCMD(m_device->CreateCommittedResource(&heapType,
                                                              D3D12_HEAP_FLAG_NONE,
                                                              &desc,
                                                              D3D12_RESOURCE_STATE_COMMON,
                                                              nullptr,
                                                              IID_PPV_ARGS(&buffer)));
            }

            // Create an upload buffer.
            ComPtr<ID3D12Resource> uploadBuffer;
            if (initialData || !immutable) {
                const auto& heapType = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
                CHECK_HRCMD(m_device->CreateCommittedResource(&heapType,
                                                              D3D12_HEAP_FLAG_NONE,
                                                              &desc,
                                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                                              nullptr,
                                                              IID_PPV_ARGS(&uploadBuffer)));
            }

            if (debugName) {
                buffer->SetName(std::wstring(debugName->begin(), debugName->end()).c_str());
            }

            auto result = std::make_shared<D3D12Buffer>(
                shared_from_this(), desc, buffer.Get(), m_rvHeap, !immutable ? uploadBuffer.Get() : nullptr);

            if (initialData) {
                result->uploadData(initialData, size, uploadBuffer.Get());
                flushContext(true);
            }

            return result;
        }

        std::shared_ptr<ISimpleMesh> createSimpleMesh(std::vector<SimpleMeshVertex>& vertices,
                                                      std::vector<uint16_t>& indices,
                                                      const std::optional<std::string>& debugName) override {
            // TODO: Implement this.
            return {};
        }

        std::shared_ptr<IQuadShader> createQuadShader(const std::string& shaderPath,
                                                      const std::string& entryPoint,
                                                      const std::optional<std::string>& debugName,
                                                      const D3D_SHADER_MACRO* defines,
                                                      const std::string includePath) override {
            ComPtr<ID3DBlob> psBytes;
            if (!includePath.empty()) {
                utilities::shader::IncludeHeader includes({includePath});
                utilities::shader::CompileShader(shaderPath, entryPoint, &psBytes, defines, &includes, "ps_5_0");
            } else {
                utilities::shader::CompileShader(shaderPath, entryPoint, &psBytes, defines, nullptr, "ps_5_0");
            }

            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.VS = {reinterpret_cast<BYTE*>(m_quadVertexShaderBytes->GetBufferPointer()),
                       m_quadVertexShaderBytes->GetBufferSize()};
            desc.PS = {reinterpret_cast<BYTE*>(psBytes->GetBufferPointer()), psBytes->GetBufferSize()};
            desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            desc.SampleMask = UINT_MAX;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            // The rest of the descriptor will be filled up by D3D12QuadShader.

            return std::make_shared<D3D12QuadShader>(shared_from_this(), desc, psBytes.Get(), debugName);
        }

        std::shared_ptr<IComputeShader> createComputeShader(const std::string& shaderPath,
                                                            const std::string& entryPoint,
                                                            const std::optional<std::string>& debugName,
                                                            const std::array<unsigned int, 3>& threadGroups,
                                                            const D3D_SHADER_MACRO* defines,
                                                            const std::string includePath) override {
            ComPtr<ID3DBlob> csBytes;
            if (!includePath.empty()) {
                utilities::shader::IncludeHeader includes({includePath});
                utilities::shader::CompileShader(shaderPath, entryPoint, &csBytes, defines, &includes, "cs_5_0");
            } else {
                utilities::shader::CompileShader(shaderPath, entryPoint, &csBytes, defines, nullptr, "cs_5_0");
            }

            D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.CS = {reinterpret_cast<BYTE*>(csBytes->GetBufferPointer()), csBytes->GetBufferSize()};
            // The rest of the descriptor will be filled up by D3D12ComputeShader.

            return std::make_shared<D3D12ComputeShader>(
                shared_from_this(), desc, csBytes.Get(), debugName, threadGroups);
        }

        std::shared_ptr<IGpuTimer> createTimer() override {
            return std::make_shared<D3D12GpuTimer>(shared_from_this());
        }

        void setShader(std::shared_ptr<IQuadShader> shader) override {
            m_currentQuadShader.reset();
            m_currentComputeShader.reset();
            m_currentRootSlot = 0;

            ID3D12DescriptorHeap* heaps[] = {
                m_rvHeap.heap.Get(),
                m_samplerHeap.heap.Get(),
            };
            m_context->SetDescriptorHeaps(ARRAYSIZE(heaps), heaps);

            auto d3d12Shader = dynamic_cast<D3D12Shader*>(shader.get());

            if (!d3d12Shader->needsResolve()) {
                // Prepare to draw the quad.
                const auto shaderData = shader->getNative<D3D12>();
                // TODO: This code is duplicated in D3D12QuadShader::resolve().
                m_context->SetGraphicsRootSignature(shaderData->rootSignature);
                m_context->SetPipelineState(shaderData->pipelineState);
                m_context->IASetIndexBuffer(nullptr);
                m_context->IASetVertexBuffers(0, 0, nullptr);
                m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                // TODO: This is somewhat restrictive, but for now we only support a linear sampler in slot 0.
                m_context->SetGraphicsRootDescriptorTable(m_currentRootSlot++,
                                                          m_samplerHeap.getGPUHandle(m_linearClampSamplerPS));
            } else {
                d3d12Shader->registerSamplerParameter(0, m_samplerHeap.getGPUHandle(m_linearClampSamplerPS));
            }

            m_currentQuadShader = shader;
        }

        void setShader(std::shared_ptr<IComputeShader> shader) override {
            m_currentQuadShader.reset();
            m_currentComputeShader.reset();
            m_currentRootSlot = 0;

            ID3D12DescriptorHeap* heaps[] = {
                m_rvHeap.heap.Get(),
                m_samplerHeap.heap.Get(),
            };
            m_context->SetDescriptorHeaps(ARRAYSIZE(heaps), heaps);

            auto d3d12Shader = dynamic_cast<D3D12Shader*>(shader.get());

            if (!d3d12Shader->needsResolve()) {
                const auto shaderData = shader->getNative<D3D12>();
                m_context->SetComputeRootSignature(shaderData->rootSignature);
                m_context->SetPipelineState(shaderData->pipelineState);
                // TODO: This is somewhat restrictive, but for now we only support a linear sampler in slot 0.
                m_context->SetComputeRootDescriptorTable(m_currentRootSlot++,
                                                         m_samplerHeap.getGPUHandle(m_linearClampSamplerCS));
            } else {
                d3d12Shader->registerSamplerParameter(0, m_samplerHeap.getGPUHandle(m_linearClampSamplerCS));
            }

            m_currentComputeShader = shader;
        }

        void setShaderInput(uint32_t slot, std::shared_ptr<ITexture> input, int32_t slice) override {
            if (m_currentQuadShader || m_currentComputeShader) {
                D3D12Shader* d3d12Shader;
                if (m_currentComputeShader) {
                    d3d12Shader = dynamic_cast<D3D12Shader*>(m_currentComputeShader.get());
                } else {
                    d3d12Shader = dynamic_cast<D3D12Shader*>(m_currentQuadShader.get());
                }

                const auto& handle =
                    *(slice == -1 ? input->getShaderInputView() : input->getShaderInputView(slice))->getNative<D3D12>();

                if (!d3d12Shader->needsResolve()) {
                    if (m_currentComputeShader) {
                        m_context->SetComputeRootDescriptorTable(m_currentRootSlot++, m_rvHeap.getGPUHandle(handle));
                    } else {
                        m_context->SetGraphicsRootDescriptorTable(m_currentRootSlot++, m_rvHeap.getGPUHandle(handle));
                    }
                } else {
                    d3d12Shader->registerSRVParameter(slot, m_rvHeap.getGPUHandle(handle));
                }
            } else {
                throw std::runtime_error("No shader is set");
            }
        }

        void setShaderInput(uint32_t slot, std::shared_ptr<IShaderBuffer> input) override {
            if (m_currentQuadShader || m_currentComputeShader) {
                D3D12Shader* d3d12Shader;
                if (m_currentComputeShader) {
                    d3d12Shader = dynamic_cast<D3D12Shader*>(m_currentComputeShader.get());
                } else {
                    d3d12Shader = dynamic_cast<D3D12Shader*>(m_currentQuadShader.get());
                }

                auto d3d12Buffer = dynamic_cast<D3D12Buffer*>(input.get());

                const auto& handle = d3d12Buffer->getConstantBufferView();

                if (!d3d12Shader->needsResolve()) {
                    if (m_currentComputeShader) {
                        m_context->SetComputeRootDescriptorTable(m_currentRootSlot++, m_rvHeap.getGPUHandle(handle));
                    } else {
                        m_context->SetGraphicsRootDescriptorTable(m_currentRootSlot++, m_rvHeap.getGPUHandle(handle));
                    }
                } else {
                    d3d12Shader->registerCBVParameter(slot, m_rvHeap.getGPUHandle(handle));
                }
            } else {
                throw std::runtime_error("No shader is set");
            }
        }

        void setShaderOutput(uint32_t slot, std::shared_ptr<ITexture> output, int32_t slice) override {
            if (m_currentQuadShader) {
                if (slot) {
                    throw new std::runtime_error("Only use slot 0 for IQuadShader");
                }
                if (slice == -1) {
                    setRenderTargets({output}, nullptr);
                } else {
                    setRenderTargets({std::make_pair(output, slice)}, {});
                }

                auto d3d12Shader = dynamic_cast<D3D12Shader*>(m_currentQuadShader.get());
                if (d3d12Shader->needsResolve()) {
                    d3d12Shader->setOutputFormat(output->getInfo());
                }
            } else if (m_currentComputeShader) {
                auto d3d12Shader = dynamic_cast<D3D12Shader*>(m_currentComputeShader.get());

                const auto& handle =
                    *(slice == -1 ? output->getComputeShaderOutputView() : output->getComputeShaderOutputView(slice))
                         ->getNative<D3D12>();

                if (!d3d12Shader->needsResolve()) {
                    m_context->SetComputeRootDescriptorTable(m_currentRootSlot++, m_rvHeap.getGPUHandle(handle));
                } else {
                    d3d12Shader->registerUAVParameter(slot, m_rvHeap.getGPUHandle(handle));
                }
            } else {
                throw std::runtime_error("No shader is set");
            }
        }

        void dispatchShader(bool doNotClear) const override {
            if (m_currentQuadShader || m_currentComputeShader) {
                {
                    D3D12Shader* d3d12Shader;
                    if (m_currentComputeShader) {
                        d3d12Shader = dynamic_cast<D3D12Shader*>(m_currentComputeShader.get());
                    } else {
                        d3d12Shader = dynamic_cast<D3D12Shader*>(m_currentQuadShader.get());
                    }

                    // The first time, we need to resolve the root signature and create the pipeline state.
                    if (d3d12Shader->needsResolve()) {
                        d3d12Shader->resolve();
                    }
                }

                if (m_currentQuadShader) {
                    m_context->DrawInstanced(3, 1, 0, 0);
                } else if (m_currentComputeShader) {
                    m_context->Dispatch(m_currentComputeShader->getThreadGroups()[0],
                                        m_currentComputeShader->getThreadGroups()[1],
                                        m_currentComputeShader->getThreadGroups()[2]);
                }
            } else {
                throw std::runtime_error("No shader is set");
            }

            if (!doNotClear) {
                m_currentQuadShader.reset();
                m_currentComputeShader.reset();
            }
        }

        void unsetRenderTargets() override {
            m_context->OMSetRenderTargets(0, nullptr, true, nullptr);

            m_currentDrawRenderTarget.reset();
            m_currentDrawDepthBuffer.reset();
        }

        void setRenderTargets(std::vector<std::shared_ptr<ITexture>> renderTargets,
                              std::shared_ptr<ITexture> depthBuffer) override {
            std::vector<std::pair<std::shared_ptr<ITexture>, int32_t>> renderTargetsNoSlice;
            for (auto renderTarget : renderTargets) {
                renderTargetsNoSlice.push_back(std::make_pair(renderTarget, -1));
            }
            setRenderTargets(renderTargetsNoSlice, std::make_pair(depthBuffer, -1));
        }

        void setRenderTargets(std::vector<std::pair<std::shared_ptr<ITexture>, int32_t>> renderTargets,
                              std::pair<std::shared_ptr<ITexture>, int32_t> depthBuffer) override {
            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
            std::vector<D3D12_RESOURCE_BARRIER> barriers;

            for (auto renderTarget : renderTargets) {
                const auto slice = renderTarget.second;

                if (slice == -1) {
                    rtvs.push_back(*renderTarget.first->getRenderTargetView()->getNative<D3D12>());
                } else {
                    rtvs.push_back(*renderTarget.first->getRenderTargetView(slice)->getNative<D3D12>());
                }

                // We assume that the resource is always in the expected state.
                // barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(renderTarget.first->getNative<D3D12>(),
                //                                                         D3D12_RESOURCE_STATE_COMMON,
                //                                                         D3D12_RESOURCE_STATE_RENDER_TARGET));
            }

            if (depthBuffer.first) {
                // We assume that the resource is always in the expected state.
                // barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(depthBuffer.first->getNative<D3D12>(),
                //                                                         D3D12_RESOURCE_STATE_COMMON,
                //                                                         D3D12_RESOURCE_STATE_DEPTH_WRITE));
            }

            if (barriers.size()) {
                m_context->ResourceBarrier((UINT)barriers.size(), barriers.data());
            }
            m_context->OMSetRenderTargets(
                (UINT)rtvs.size(),
                rtvs.data(),
                false,
                depthBuffer.first ? depthBuffer.first->getDepthStencilView()->getNative<D3D12>() : nullptr);

            if (renderTargets.size() > 0) {
                m_currentDrawRenderTarget = renderTargets[0].first;
                m_currentDrawRenderTargetSlice = renderTargets[0].second;
                m_currentDrawDepthBuffer = depthBuffer.first;
                m_currentDrawDepthBufferSlice = depthBuffer.second;

                const auto viewport = CD3DX12_VIEWPORT(0.0f,
                                                       0.0f,
                                                       (float)m_currentDrawRenderTarget->getInfo().width,
                                                       (float)m_currentDrawRenderTarget->getInfo().height);
                m_context->RSSetViewports(1, &viewport);

                const auto scissorRect = CD3DX12_RECT(
                    0, 0, m_currentDrawRenderTarget->getInfo().width, m_currentDrawRenderTarget->getInfo().height);
                m_context->RSSetScissorRects(1, &scissorRect);
            } else {
                m_currentDrawRenderTarget.reset();
                m_currentDrawDepthBuffer.reset();
            }
        }

        void clearColor(float top, float left, float bottom, float right, XrColor4f& color) const override {
            if (!m_currentDrawRenderTarget) {
                return;
            }

            // When rendering text, we must use the corresponding device.
            if (!m_isRenderingText) {
                D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView;
                if (m_currentDrawRenderTargetSlice == -1) {
                    renderTargetView = *m_currentDrawRenderTarget->getRenderTargetView()->getNative<D3D12>();
                } else {
                    renderTargetView = *m_currentDrawRenderTarget->getRenderTargetView(m_currentDrawRenderTargetSlice)
                                            ->getNative<D3D12>();
                }

                float clearColor[] = {color.r, color.g, color.b, color.a};
                const auto rect = CD3DX12_RECT((LONG)left, (LONG)top, (LONG)right, (LONG)bottom);
                m_context->ClearRenderTargetView(renderTargetView, clearColor, 1, &rect);
            } else {
                m_textDevice->clearColor(top, left, bottom, right, color);
            }
        }

        void clearDepth(float value) override {
            if (!m_currentDrawDepthBuffer) {
                return;
            }

            D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView;
            if (m_currentDrawDepthBufferSlice == -1) {
                depthStencilView = *m_currentDrawDepthBuffer->getDepthStencilView()->getNative<D3D12>();
            } else {
                depthStencilView =
                    *m_currentDrawDepthBuffer->getDepthStencilView(m_currentDrawDepthBufferSlice)->getNative<D3D12>();
            }

            m_context->ClearDepthStencilView(depthStencilView, D3D12_CLEAR_FLAG_DEPTH, value, 0, 0, nullptr);
        }

        void setViewProjection(const XrPosef& eyePose, const XrFovf& fov, float depthNear, float depthFar) override {
            // TODO: Implement this.
        }

        void draw(std::shared_ptr<ISimpleMesh> mesh, const XrPosef& pose, XrVector3f scaling) override {
            // TODO: Implement this.
        }

        float drawString(std::wstring string,
                         TextStyle style,
                         float size,
                         float x,
                         float y,
                         uint32_t color,
                         bool measure,
                         bool alignRight) override {
            return m_textDevice->drawString(string, style, size, x, y, color, measure, alignRight);
        }

        float drawString(std::string string,
                         TextStyle style,
                         float size,
                         float x,
                         float y,
                         uint32_t color,
                         bool measure,
                         bool alignRight) override {
            return m_textDevice->drawString(string, style, size, x, y, color, measure, alignRight);
        }

        float measureString(std::wstring string, TextStyle style, float size) const override {
            return m_textDevice->measureString(string, style, size);
        }

        float measureString(std::string string, TextStyle style, float size) const override {
            return m_textDevice->measureString(string, style, size);
        }

        void beginText() override {
            // Grab the interop version of the render target texture...
            auto d3d12DrawRenderTarget = dynamic_cast<D3D12Texture*>(m_currentDrawRenderTarget.get());
            m_currentTextRenderTarget = d3d12DrawRenderTarget->getInteropTexture();
            if (!m_currentTextRenderTarget) {
                // ...or create it if needed.
                ComPtr<ID3D11Texture2D> interopTexture;
                D3D11_RESOURCE_FLAGS flags;
                ZeroMemory(&flags, sizeof(flags));
                flags.BindFlags = D3D11_BIND_RENDER_TARGET;
                CHECK_HRCMD(m_textInteropDevice->CreateWrappedResource(m_currentDrawRenderTarget->getNative<D3D12>(),
                                                                       &flags,
                                                                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                       IID_PPV_ARGS(&interopTexture)));

                m_currentTextRenderTarget = WrapD3D11Texture(m_textDevice,
                                                             m_currentDrawRenderTarget->getInfo(),
                                                             interopTexture.Get(),
                                                             "Render Target Interop TEX2D");

                d3d12DrawRenderTarget->setInteropTexture(m_currentTextRenderTarget);
            }
            {
                ID3D11Resource* resources[] = {m_currentTextRenderTarget->getNative<D3D11>()};
                m_textInteropDevice->AcquireWrappedResources(resources, 1);
            }

            // Setup the interop context for rendering.
            m_textDevice->setRenderTargets({std::make_pair(m_currentTextRenderTarget, m_currentDrawRenderTargetSlice)});
            m_textDevice->beginText();
            m_isRenderingText = true;
        }

        void flushText() override {
            m_textDevice->flushText();
            m_textDevice->unsetRenderTargets();
            // Commit to the D3D12 queue.
            m_textDevice->flushContext(true);
            {
                ID3D11Resource* resources[] = {m_currentTextRenderTarget->getNative<D3D11>()};
                m_textInteropDevice->ReleaseWrappedResources(resources, 1);
            }
            m_currentTextRenderTarget.reset();
            m_isRenderingText = false;
        }

        uint32_t getBufferAlignmentConstraint() const override {
            return D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        }

        uint32_t getTextureAlignmentConstraint() const override {
            return D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        }

        void* getNativePtr() const override {
            return m_device.Get();
        }

        void* getContextPtr() const override {
            return m_context.Get();
        }

      private:
        // Initialize the resources needed for dispatchShader() and related calls.
        void initializeShadingResources() {
            {
                D3D12_SAMPLER_DESC desc;
                ZeroMemory(&desc, sizeof(desc));
                desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                desc.MaxAnisotropy = 1;
                desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                m_samplerHeap.allocate(m_linearClampSamplerPS);
                m_device->CreateSampler(&desc, m_linearClampSamplerPS);
            }
            {
                D3D12_SAMPLER_DESC desc;
                ZeroMemory(&desc, sizeof(desc));
                desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                desc.MaxAnisotropy = 1;
                desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                desc.MinLOD = D3D12_MIP_LOD_BIAS_MIN;
                desc.MaxLOD = D3D12_MIP_LOD_BIAS_MAX;
                m_samplerHeap.allocate(m_linearClampSamplerCS);
                m_device->CreateSampler(&desc, m_linearClampSamplerCS);
            }
            {
                ComPtr<ID3DBlob> errors;
                const HRESULT hr = D3DCompile(QuadVertexShader.c_str(),
                                              QuadVertexShader.length(),
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              "vsMain",
                                              "vs_5_0",
                                              D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS,
                                              0,
                                              &m_quadVertexShaderBytes,
                                              &errors);
                if (FAILED(hr)) {
                    if (errors) {
                        Log("%s", (char*)errors->GetBufferPointer());
                    }
                    CHECK_HRESULT(hr, "Failed to compile shader");
                }
            }
        }

        ComPtr<ID3D12Device> m_device;
        ComPtr<ID3D12CommandQueue> m_queue;
        std::string m_deviceName;

        ComPtr<ID3D12CommandAllocator> m_commandAllocator[NumInflightContexts];
        ComPtr<ID3D12GraphicsCommandList> m_commandList[NumInflightContexts];
        uint32_t m_currentContext{0};

        ComPtr<ID3D12GraphicsCommandList> m_context;
        D3D12Heap m_rtvHeap;
        D3D12Heap m_dsvHeap;
        D3D12Heap m_rvHeap;
        D3D12Heap m_samplerHeap;
        ComPtr<ID3D12QueryHeap> m_queryHeap;
        ComPtr<ID3DBlob> m_quadVertexShaderBytes;
        D3D12_CPU_DESCRIPTOR_HANDLE m_linearClampSamplerPS;
        D3D12_CPU_DESCRIPTOR_HANDLE m_linearClampSamplerCS;
        ComPtr<ID3D12Fence> m_fence;
        UINT64 m_fenceValue{0};

        double m_gpuTickDelta{0};

        std::shared_ptr<IDevice> m_textDevice;
        ComPtr<ID3D11On12Device> m_textInteropDevice;
        bool m_isRenderingText{false};

        std::shared_ptr<ITexture> m_currentTextRenderTarget;
        std::shared_ptr<ITexture> m_currentDrawRenderTarget;
        int32_t m_currentDrawRenderTargetSlice;
        std::shared_ptr<ITexture> m_currentDrawDepthBuffer;
        int32_t m_currentDrawDepthBufferSlice;

        mutable std::shared_ptr<IQuadShader> m_currentQuadShader;
        mutable std::shared_ptr<IComputeShader> m_currentComputeShader;
        uint32_t m_currentRootSlot;

        friend std::shared_ptr<ITexture>
        toolkit::graphics::WrapD3D12Texture(std::shared_ptr<IDevice> device,
                                            const XrSwapchainCreateInfo& info,
                                            ID3D12Resource* texture,
                                            const std::optional<std::string>& debugName);
    };

} // namespace

namespace toolkit::graphics {
    std::shared_ptr<IDevice> WrapD3D12Device(ID3D12Device* device, ID3D12CommandQueue* queue) {
        return std::make_shared<D3D12Device>(device, queue);
    }

    std::shared_ptr<ITexture> WrapD3D12Texture(std::shared_ptr<IDevice> device,
                                               const XrSwapchainCreateInfo& info,
                                               ID3D12Resource* texture,
                                               const std::optional<std::string>& debugName) {
        if (device->getApi() != Api::D3D12) {
            throw new std::runtime_error("Not a D3D12 device");
        }
        auto d3d12Device = dynamic_cast<D3D12Device*>(device.get());

        if (debugName) {
            texture->SetName(std::wstring(debugName->begin(), debugName->end()).c_str());
        }

        const D3D12_RESOURCE_DESC desc = texture->GetDesc();
        return std::make_shared<D3D12Texture>(
            device, info, desc, texture, d3d12Device->m_rtvHeap, d3d12Device->m_dsvHeap, d3d12Device->m_rvHeap);
    }

} // namespace toolkit::graphics

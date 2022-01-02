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

namespace {

    using namespace toolkit;
    using namespace toolkit::graphics;
    using namespace toolkit::log;

    class D3D11DepthRetriever : public IDepthRetriever {
      public:
        D3D11DepthRetriever(std::shared_ptr<IDevice> graphicsDevice) : m_device(graphicsDevice) {
        }

        void registerRenderTarget(std::shared_ptr<ITexture> renderTarget,
                                  std::function<void(std::shared_ptr<ITexture>, bool)> callback) override {
        }

      private:
        const std::shared_ptr<IDevice> m_device;
    };

} // namespace

namespace toolkit::graphics {

    std::shared_ptr<IDepthRetriever> CreateDepthRetriever(std::shared_ptr<IDevice> graphicsDevice) {
        if (graphicsDevice->getApi() == Api::D3D11) {
            return std::make_shared<D3D11DepthRetriever>(graphicsDevice);
        }

        throw new std::runtime_error("Unsupported graphics runtime");
    }

} // namespace toolkit::graphics

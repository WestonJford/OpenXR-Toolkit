---
layout: default
title: FAQ
nav_order: 6
---

{:toc}

## Q: ELI5: What is OpenXR?

OpenXR is a structured set of instructions and rules for developers to create applications (such as Flight Simulator 2020) that use virtual reality or augmented reality (or XR as the industry calls it) that run on modern devices (such as the HP Reverb or Oculus Quest).

## Q: What headset does it work with?

It should work with any VR headset thanks to OpenXR. We've seen success with Windows Mixed Reality (eg: HP Reverb), any headset going through the SteamVR runtime (Valve Index), Pimax, Oculus Quest...

## Q: What GPUs does it work with?

Any GPU that is compatible DirectX 11.

Yes, even the Nvidia Image Scaling (NIS) will work on non-Nvidia cards, and AMD's FSR will work on non-AMD cards.

## Q: Does it work with DX11 and DX12?

It works with both, however DX12 support is considered experimental at this time.

## Q: Will it work with other games like the ones from Steam?

This software only works with OpenXR applications, not OpenVR applications.

Even with OpenXR applications, I cannot guarantee it will work, as I've only implemented the bare minimum for MSFS.

## Q: Do I need to run any application?

No, you may just run your game as usual. No need to open he companion app.

## Q: Can you tell me what the best settings are?

No, I cannot. The settings depend on your hardware and your expectations.

## Q: There’s already a NIS option in my GPU driver... Do I need this?

The NIS option in the NVIDIA drivers does not apply to VR. You need this software to use NIS with VR. It is also recommended to turn off NIS in the NVIDIA driver when using this software, otherwise the GPU will be performing extra processing for the desktop view and not for VR.

## Q: I am not getting better performance from upscaling.

The upscaling feature will help relieve your GPU, which will only yield better performance if your GPU was the limiting component. If your performance is limited by your CPU, upscaling will not help improving performance.

## Q: How can I compare the image quality?

You can use the companion app to enable the screenshot mode. You may then press Ctrl+F12 in game to capture the left eye image, and have it stored in your `%LocalAppData%` folder. You may then use a tool such as the [Image Comparison Analysis Tool (ICAT)](https://www.nvidia.com/en-us/geforce/technologies/icat/) from Nvidia to compare images.

## Q: Is this affiliated with Microsoft?

While I am a Microsoft employee working on OpenXR, please note that this is a personal project not affiliated with Microsoft.

## Q: Is it open source? Can I contribute?

It is 100% open source, and if you would like to contribute I am happy if you get in touch with me!

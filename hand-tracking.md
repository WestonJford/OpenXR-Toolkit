---
layout: default
title: Hand Tracking
parent: Features
nav_order: 2
---

{:toc}

## Overview

The OpenXR Toolkit does not add hand tracking capability to your headset, but instead it leverages hand tracking capabilities of some devices (such as the Leap Motion controller) and enable the hand tracking to act as VR controllers.

Devices confirmed to work:

* Ultraleap [Leap Motion Controller](https://www.ultraleap.com/product/leap-motion-controller/)
* Ultraleap [Stereo IR 170](https://www.ultraleap.com/product/stereo-ir-170/)
* Pimax [Hand Tracking Module](https://pimax.com/product/hand-tracking-module/)

We would love to add more devices to this list! If you have a device that supports hand tracking via OpenXR that is no on this list, please contact us on [Discord](https://discord.gg/WXFshwMnke) or submit an [issue on GitHub](https://github.com/mbucchia/OpenXR-Toolkit/issues).

## Using with Ultraleap or Pimax devices

1. Download and install the [Leap Motion tracking software](https://developer.leapmotion.com/tracking-software-download).

2. Use the included Visualizer app to confirm that the Leap Motion Controller is properly setup and functional.

3. Download and install the [Ultraleap OpenXR Hand Tracking API Layer](https://github.com/ultraleap/OpenXRHandTracking).

4. With your game running, open the menu (Ctrl+F2), then navigate to **Hand Tracking**. Select either **Both** to use both hands, or **Left**/**Right** to use only one hand. Restart the VR session for the hand tracking to begin.

## Customizing the tracking and gestures

# Welcome

This software provides a collection of useful features to customize and improve existing OpenXR applications.

This includes image upscaling using the NVIDIA Image Scaling (NIS) algorithm (available with all GPUs, including non-NVIDIA) and other features to adjust and enhance your VR experience.

DISCLAIMER: This software is distributed as-is, without any warranties or conditions of any kind. Use at your own risks.

Please report bugs here: https://github.com/mbucchia/OpenXR-Toolkit/issues.

# Setup

## Requirements

This software may be used with any brand of VR headset as long as the target application is developed using OpenXR. This software may be used with any GPU compatible with DirectX 11 and above.

## Limitations

+ This software was only extensively tested with Microsoft Flight Simulator 2020;
+ Only Direct3D 11 is supported;
+ See all [open issues](https://github.com/mbucchia/OpenXR-Toolkit/issues).

## Installation

Download the installer package from the [release page](https://github.com/mbucchia/OpenXR-Toolkit/releases).

Run the `OpenXR-Toolkit.msi` program.

![Installer file](site/installer-file.png)

Follow the instructions to complete the installation procedure.

![Setup wizard](site/installer.png)

Once installed, you may use the _OpenXR Toolkit Companion app_ (found on the desktop or Start menu) to confirm that the software is active.

![Companion app shortcut](site/companion-start.png)

The _OpenXR Toolkit Companion app_ will display a green or red status indicating whether the software is activated.

![Companion app](site/companion.png)

You may enable or disable some advanced features from the _OpenXR Toolkit Companion app_. However, all the settings for the toolkit are only available from within your OpenXR application (see below).

## Using the toolkit

Once installed, please run the desired OpenXR application and use the Ctrl+F1 key combination to enter the configuration menu.

## Removing

The software can be removed from Windows' _Add or remove programs_ menu.

![Add or remove programs](site/add-or-remove.png)

In the list of applications, select _OpenXR-Toolkit_, then click _Uninstall_.

![Uninstall](site/uninstall.png)

## Contributions

The author is Matthieu Bucchianeri (https://github.com/mbucchia/). Please note that this software is not affiliated with Microsoft.

Many thanks to the https://forums.flightsimulator.com/ community for the testing and feedback!

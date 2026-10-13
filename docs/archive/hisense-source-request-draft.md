# A6L kernel source request — unsent draft

No recipient has been verified and no message has been sent. This draft contains
model/build information only, without device identifiers or private firmware.

## English

Subject: Hisense A6L / HLTE730T — request for matching Linux kernel source

Hello,

I own a Hisense A6L (HLTE730T) and would like to maintain its Android software
while preserving both its LCD and rear e-ink display. Could you please provide
the matching Linux kernel source package, or direct me to the team responsible
for the phone's open-source releases?

The phone reports Android 9 build L1632.6.01.04 and Linux 4.4.153-perf, built
on 14 October 2020. Its recovered configuration identifies product hlte730t,
platform sdm660_overlay, and CONFIG_FB_HS_MDSS_EPD_PANEL=y. The e-ink panel
is identified as eink,ed052tc2.

I am looking for the kernel source and Hisense changes, matching defconfig,
device trees and overlays, and the build instructions/toolchain version. The
Hisense MDSS e-ink integration, SPI flash driver and board power/touch support
are particularly important. Matching source for the supplied Qualcomm Wi-Fi
kernel module would also help.

If this exact build is unavailable, please identify the nearest available A6L
source release and its corresponding firmware version. A source archive or
repository would be useful; a compiled firmware image alone cannot be used to
rebuild these drivers.

Thank you.

## 中文

主题：申请海信 A6L（HLTE730T）对应的 Linux 内核源代码

您好：

我是海信 A6L（HLTE730T）手机用户，希望维护和更新该手机的 Android 系统，
同时保留正面的 LCD 屏幕和背面的电子墨水屏功能。请问能否提供与该机型对应的
Linux 内核源代码包，或协助转交负责手机开源代码发布的团队？

手机报告的系统版本为 Android 9，版本号 L1632.6.01.04，内核版本为
4.4.153-perf，编译日期为 2020 年 10 月 14 日。提取的内核配置包含产品名称
hlte730t、平台名称 sdm660_overlay，以及 CONFIG_FB_HS_MDSS_EPD_PANEL=y。
电子墨水屏的设备标识为 eink,ed052tc2。

希望获取的内容包括内核源代码及海信修改、对应的 defconfig、设备树及其覆盖文件、
编译说明和工具链版本。其中，海信的 MDSS 墨水屏适配、SPI 闪存驱动，以及该机型
的供电和触摸控制代码尤其重要。随系统提供的高通 Wi-Fi 内核模块对应的源代码
也会有帮助。

如果无法提供此版本，请告知最接近的 A6L 源代码版本及其对应的固件版本。
源代码压缩包或代码仓库均可；仅有编译后的固件镜像无法用于重新编译这些驱动。

谢谢！

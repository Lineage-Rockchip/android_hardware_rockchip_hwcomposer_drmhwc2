# **DrmHwc2-Env-XML配置文件说明**

文件标识：RK-PC-YF-0003

发布版本：V1.0.0

日期：2024-12-02

文件密级：□绝密   □秘密   □内部资料   ■公开

---

**免责声明**

本文档按“现状”提供，瑞芯微电子股份有限公司（“本公司”，下同）不对本文档的任何陈述、信息和内容的准确性、可靠性、完整性、适销性、特定目的性和非侵权性提供任何明示或暗示的声明或保证。本文档仅作为使用指导的参考。

由于产品版本升级或其他原因，本文档将可能在未经任何通知的情况下，不定期进行更新或修改。

**商标声明**

“Rockchip”、“瑞芯微”、“瑞芯”均为本公司的注册商标，归本公司所有。

本文档可能提及的其他所有注册商标或商标，由其各自拥有者所有。

**版权所有** **© 2024** **瑞芯微电子股份有限公司**

超越合理使用范畴，非经本公司书面许可，任何单位和个人不得擅自摘抄、复制本文档内容的部分或全部，并不得以任何形式传播。

瑞芯微电子股份有限公司

Rockchip Electronics Co., Ltd.

地址：     福建省福州市铜盘路软件园A区18号

网址：     [www.rock-chips.com](http://www.rock-chips.com)

客户服务电话： +86-4007-700-590

客户服务传真： +86-591-83951833

客户服务邮箱： [fae@rock-chips.com](mailto:fae@rock-chips.com)

---

**前言**

本文档 SingleDisplayMode 功能介绍与配置说明

**读者对象**

本文档主要适用于以下工程师：

- 技术支持工程师
- 软件开发工程师

**修订记录**

| **日期**   | **版本** | **作者**  | **修改说明** |
| ---------- | -------- | --------- | ------------ |
| 2024/12/2 | 1.0.0    | 李斌 |  v1.5.173： 初版发布    |
| 2025/2/11| 1.1.0    | 李斌 | v1.5.175：修复开机黑屏问题/实现异常热插拔事件处理逻辑     |

**目 录**

[TOC]

## 1 概述

 SingleDisplayMode 功能主要的应用场景，如平板产品：存在外接HDMI/DP显示接口需求，对于单VP芯片平台（RK3566/RK3576s等）需要将VP资源动态从build-in display切换到external display上，从平板显示切换到外界显示器的显示模式。

 典型的产品：任天堂Switch掌机与主机形态切换。

**平台支持说明**

| **芯片名称** | **系统版本**     | **DrmHwc2版本**|
| ------------ | ---------------- |--|
| RK3588       | Android  12 / 13 / 14 |v1.5.175|
| RK3576       | Android  14 |v1.5.175|
| RK3568       | Android  11 / 12 / 13 / 14 |v1.5.175|
| RK3566       | Android  11 / 12 / 13 / 14 |v1.5.175|
| RK3562       | Android  13 / 14 |v1.5.175|
| RK3326       | Android  14 |v1.5.175|
| RK3399       | Android  14 | v1.5.175|
| RK3528       | Android  14 | v1.5.175|

 DrmHwc2 版本号获取命令如下：

```shell
$ adb shell getprop | grep ghwc
[vendor.ghwc.version]: [HWC2-1.5.175]
```

## 2 功能说明

单主屏模式通过修改已向SurfaceFlinger注册的主屏（display-id=0）的屏幕的底层显示资源的绑定关系，实现主屏从 build-in 到 external 切换。

下面以DSI+HDMI-A屏幕举例，说明单主屏模式切换需要满足的条件：
- 热插拔事件：触发DRM热插拔事件（插入或者拔出），即对应触发屏幕需要由底层上报热插拔事件；
- 高优先级：高优先级的屏幕，可以将低优先级的屏幕抢占。比如HDMI-A设置为高优先级后，每次HDMI-A插入，都会抢占低优先级的DSI屏幕硬件资源；

满足上述条件后，根据HDMI-A的热插拔行为，即可实现以下显示模式切换：
- 插入事件：HDMI-A插入后，主屏切换到HDMI-A；
- 拔出事件：HDMI-A拔出后，主屏切换到DSI；

注意事项：
1. 每次主屏出现切换行为后，HWC会上发热插拔事件通知SurfaceFlinger更新主屏分辨率与刷新率，以适配新接入的主屏设备；
2. 模式使能后，HWC将禁止上报除主屏以外的屏幕的热插拔事件到SurfaceFlinger，所以模式使能后 dumpsys SurfaceFlinger 始终只会看到一个屏幕；

## 3 配置说明
### 3.1 DTS配置

由于此方案多数应用于单VP芯片平台，故建议将所有需要参与VP抢占资源的屏幕都绑定到同一个VP资源上，以RK3576-EVB1为例，参考修改如下：由于HDMI默认绑定VP0，故将DSI也绑定到VP0，实现资源抢占:
```diff
kernel-6.1$ git diff
diff --git a/arch/arm64/boot/dts/rockchip/rk3576-evb.dtsi b/arch/arm64/boot/dts/rockchip/rk3576-evb.dtsi
index 82abbaf00477..5691061c2470 100644
--- a/arch/arm64/boot/dts/rockchip/rk3576-evb.dtsi
+++ b/arch/arm64/boot/dts/rockchip/rk3576-evb.dtsi
@@ -588,11 +588,11 @@ dsi_out_panel: endpoint {
 };

 &dsi_in_vp0 {
-       status = "disabled";
+       status = "okay";
 };

 &dsi_in_vp1 {
-       status = "okay";
+       status = "disabled";
 };

```

### 3.2 属性配置
```shell

# /vendor/build.prop 加入以下字段
# vendor.hwc.enable_single_display_mode=1 表示使能单主屏模式
vendor.hwc.enable_single_display_mode=1
# vendor.hwc.device.primary 是配置主屏优先级，如下配置，HDMI-A 优先级高于DSI
vendor.hwc.device.primary=HDMI-A,DSI

```
配置完成后reboot重启验证。

## 验证说明：

1. 配置信息确认：
```shell
# HWC版本号确认，确认更新为 v1.5.175
adb shell getprop | grep ghwc

# DTS配置确认，通过检查 Encoders 的 possible crtcs 字段确认
adb shell modetest -e

# 属性配置确认，检查配置是否符合预期
adb shell getprop vendor.hwc.enable_single_display_mode
adb shell getprop vendor.hwc.device.primary
```

2. 不接HDMI-A开机，确认开机的主屏注册信息
```shell
# 确认当前配置的主屏屏幕类型， 输出为 [DSI-1:100:connected]
adb shell getprop vendor.hwc.device.display-0
# 系统渲染分辨率为 1080x1920
adb shell wm size

```

3. 插入 HDMI-A，查看日志与注册情况：
```shell
adb shell logcat | grep -E "hotplug|DisplayPipeChange"

## 日志输出如下：

# 检测到HDMI-A-1的插入事件：
## 收到HDMI热插拔插入事件，将HDMI设置为主屏
I hwc-drm-two: HandleEvent,line=5810 hwc_hotplug : HDMI-A-1 state is changed! Disconnected -> Connected, timestamp_us=1087728631779
I hwc-drm-two: hwc_hotplug: Plug event 1087728631779 for connector 412 type=HDMI-A, type_id=1
## 插入事件，进行切换主屏模式
I hwc-drm-two: HandleSingleDisplayEvent,line=5908 hwc_hotplug: Plug connector 412 type=HDMI-A type_id=1 to switch primary config.
## 复用主屏切换逻辑，将HDMI设置为主屏，并且上报SurfaceFlinger，完成切换
I hwc-drm-two: SendDrmEvent,line=6345 hwc_hotplug : add event display-id=1 type=2 connection=connected timestamp=59394522492570
I hwc-drm-two: HandleEvent,line=5853 hwc_hotplug : HDMI-A-1 state is changed! Disconnected -> Connected, timestamp_us=59394522492570
I hwc-drm-two: HandleEvent,line=5859 hwc_hotplug : HDMI-A-2 state is no changed! Disconnected -> Disconnected, timestamp_us=59394522492570
I hwc-drm-two: HandleEvent,line=5859 hwc_hotplug : DSI-1 state is no changed! Connected -> Connected, timestamp_us=59394522492570
I hwc-drm-two: HandleEvent,line=5859 hwc_hotplug : DP-1 state is no changed! Disconnected -> Disconnected, timestamp_us=59394522492570
I hwc-drm-two: HandleEvent,line=5859 hwc_hotplug : DP-2 state is no changed! Disconnected -> Disconnected, timestamp_us=59394522492570
I hwc-drm-two: HandleSingleDisplayEvent,line=5955 hwc_hotplug: Plug event 59394522492570 for connector 479 type=HDMI-A, type_id=1
I hwc-drm-two: HandleSingleDisplayEvent,line=5982 hwc_hotplug: primaryChange, old_primary=0 DSI-1, new_primary=1 HDMI-A-1
I hwc-drm-two: HandleSingleDisplayEvent,line=6008 hwc_hotplug: Plug connector 479 type=HDMI-A type_id=1 to switch primary config.

```

4. 查看主屏注册情况：
```shell
adb shell getprop vendor.hwc.device.display-0
## 输出为 HDMI-A-1:72:connected
adb shell wm size
# 系统渲染分辨率为 1920x1080
```

5. 拔出HDMI-A,查看日志与注册情况
```shell
adb shell logcat | grep -E "hotplug|DisplayPipeChange"

## 日志输出如下：
## 收到HDMI热插拔拔出事件，将DSI设置为主屏
I hwc-drm-two: SendDrmEvent,line=6345 hwc_hotplug : add event display-id=0 type=2 connection=disconnected timestamp=59399583982073
I hwc-drm-two: HandleEvent,line=5853 hwc_hotplug : HDMI-A-1 state is changed! Connected -> Disconnected, timestamp_us=59399583982073
I hwc-drm-two: HandleEvent,line=5859 hwc_hotplug : HDMI-A-2 state is no changed! Disconnected -> Disconnected, timestamp_us=59399583982073
I hwc-drm-two: HandleEvent,line=5859 hwc_hotplug : DSI-1 state is no changed! Connected -> Connected, timestamp_us=59399583982073
I hwc-drm-two: HandleEvent,line=5859 hwc_hotplug : DP-1 state is no changed! Disconnected -> Disconnected, timestamp_us=59399583982073
I hwc-drm-two: HandleEvent,line=5859 hwc_hotplug : DP-2 state is no changed! Disconnected -> Disconnected, timestamp_us=59399583982073
I hwc-drm-two: HandleSingleDisplayEvent,line=5955 hwc_hotplug: Unplug event 59399583982073 for connector 479 type=HDMI-A, type_id=1
I hwc-drm-two: HandleSingleDisplayEvent,line=5982 hwc_hotplug: primaryChange, old_primary=0 HDMI-A-1, new_primary=2 DSI-1
I hwc-drm-two: HandleSingleDisplayEvent,line=6008 hwc_hotplug: Unplug connector 479 type=HDMI-A type_id=1 to switch primary config.

```

4. 查看HDMI-A注册情况：
```shell
adb shell getprop vendor.hwc.device.display-0
## 输出为 DSI-1:144:connected
adb shell wm size
# 系统渲染分辨率为 1080x1920

```


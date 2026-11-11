/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp.c
 * @brief QF bring-up: publish/subscribe bus and the event pools.
 */
#include "kqp.h"
#include "kqp_bsp.h"
#include "kqp_worker.h"
#include "sdkconfig.h"
#if CONFIG_KQP_AO_WATCH
#include "kqp_watch_judge.h"
#endif

/** Subscriber storage: one QSubscrList per published signal. */
static QSubscrList s_subscr_sto[MAX_PUB_SIG];

/* Event pools. QF requires the pools to be registered in order of strictly
 * increasing block size, and it serves an allocation from the smallest pool
 * whose block still fits. Four tiers:
 *   - small  : LED commands, touch samples, console bytes
 *   - medium : pixel flushes, console lines, progress reports (they name the
 *              file being written) and the end of a write (it names the app
 *              installed), app-install requests and entries
 *   - large  : log lines - a whole formatted console line each
 *   - jumbo  : RecvDataEvt and AppDataIconEvt, the two things 4 KB wide
 *
 * A block is a WIDTH, not a container. Declaring one in C needs a type, so
 * each tier is typedef'd from the widest event that has to fit in it: the
 * small tier is as wide as a TouchEvt, and every event narrower than that is
 * served from it too, using a whole block and wasting the difference. Read
 * KQP_BLOCK_SMALL below as "the small tier's width" and never as "a touch
 * sample" - which tier an event lands in is decided by its size alone.
 */
typedef TouchEvt       KqpSmallBlock;
typedef ConsoleLineEvt KqpMediumBlock;
/* The large tier is as wide as the widest of its kinds of tenant: a log
 * line, an MQTT message now that its payload is a whole console line, and
 * the scheduler's entry (a name, an expression and a line, and a 64-bit time
 * that pads its header to eight bytes). The entry is the widest; the others'
 * fit is asserted below. */
#if CONFIG_KODE_SCHEDULER
typedef SchedEntryEvt  KqpLargeBlock;
#else
typedef MqttMsgEvt     KqpLargeBlock;
#endif
/* AppDataIconEvt carries project[32] on top of the same 4096-byte payload
 * RecvDataEvt has, so it is wider - the jumbo tier is typedef'd from it now,
 * and RecvDataEvt gets the explicit fit check below that it used to be
 * exempt from by definition. */
typedef AppDataIconEvt KqpJumboBlock;

#define KQP_BLOCK_SMALL   sizeof(KqpSmallBlock)
#define KQP_BLOCK_MEDIUM  sizeof(KqpMediumBlock)
#define KQP_BLOCK_LARGE   sizeof(KqpLargeBlock)
#define KQP_BLOCK_JUMBO   sizeof(KqpJumboBlock)

static KqpSmallBlock   s_small_pool[KQP_SMALL_POOL_COUNT];
static KqpMediumBlock  s_medium_pool[KQP_MEDIUM_POOL_COUNT];
static KqpLargeBlock   s_large_pool[KQP_LARGE_POOL_COUNT];
static KqpJumboBlock   s_jumbo_pool[KQP_JUMBO_POOL_COUNT];

_Static_assert((KQP_TICKS_PER_SEC % 1000U) == 0U,
               "KQP_TICKS_PER_MS() is exact only for a whole number of kHz");
_Static_assert(KQP_BLOCK_SMALL < KQP_BLOCK_MEDIUM,
               "QP pools must have strictly increasing block sizes");
/* BleStateEvt outgrew the small pool when it learned to report the console,
 * so it is a medium-pool tenant now. Nothing recorded that before. */
_Static_assert(sizeof(BleStateEvt) <= KQP_BLOCK_MEDIUM,
               "BleStateEvt must fit the medium pool");
_Static_assert(KQP_BLOCK_MEDIUM < KQP_BLOCK_LARGE,
               "QP pools must have strictly increasing block sizes");
_Static_assert(sizeof(LogEvt) <= KQP_BLOCK_LARGE,
               "LogEvt must fit the large pool");
_Static_assert(KQP_BLOCK_LARGE < KQP_BLOCK_JUMBO,
               "QP pools must have strictly increasing block sizes");
_Static_assert(sizeof(BleAppWriteEvt) <= KQP_BLOCK_LARGE,
               "BleAppWriteEvt must fit the large pool");
_Static_assert(sizeof(BleAppValueReqEvt) <= KQP_BLOCK_LARGE,
               "BleAppValueReqEvt must fit the large pool");
_Static_assert(sizeof(BleAppLinkEvt) <= KQP_BLOCK_SMALL,
               "BleAppLinkEvt must fit the small pool");
_Static_assert(sizeof(KodeConfigReqEvt) <= KQP_BLOCK_MEDIUM,
               "KodeConfigReqEvt must fit the medium pool");
_Static_assert(sizeof(KodeConfigAnsEvt) <= KQP_BLOCK_MEDIUM,
               "KodeConfigAnsEvt must fit the medium pool");
_Static_assert(sizeof(ChannelStateEvt) <= KQP_BLOCK_MEDIUM,
               "ChannelStateEvt must fit the medium pool");
_Static_assert(sizeof(IdSignReqEvt) <= KQP_BLOCK_MEDIUM,
               "IdSignReqEvt must fit the medium pool");
_Static_assert(sizeof(IdSignRspEvt) <= KQP_BLOCK_MEDIUM,
               "IdSignRspEvt must fit the medium pool");
_Static_assert(sizeof(IdReqEvt) <= KQP_BLOCK_MEDIUM,
               "IdReqEvt must fit the medium pool");
_Static_assert(sizeof(IdPartEvt) <= KQP_BLOCK_MEDIUM,
               "IdPartEvt must fit the medium pool");
_Static_assert(sizeof(IdEndEvt) <= KQP_BLOCK_SMALL,
               "IdEndEvt must fit the small pool");
_Static_assert(sizeof(C5WipeReqEvt) <= KQP_BLOCK_SMALL,
               "C5WipeReqEvt must fit the small pool");
_Static_assert(sizeof(C5WipeRspEvt) <= KQP_BLOCK_SMALL,
               "C5WipeRspEvt must fit the small pool");
_Static_assert(sizeof(RadioParkEvt) <= KQP_BLOCK_SMALL,
               "RadioParkEvt must fit the small pool");
_Static_assert(sizeof(LedEvt) <= KQP_BLOCK_SMALL,
               "LedEvt must fit the small pool");
_Static_assert(sizeof(NfcTagEvt) <= KQP_BLOCK_MEDIUM,
               "NfcTagEvt must fit the medium pool");
_Static_assert(sizeof(NfcReadEvt) <= KQP_BLOCK_MEDIUM,
               "NfcReadEvt must fit the medium pool");
_Static_assert(sizeof(NfcNdefEvt) <= KQP_BLOCK_MEDIUM,
               "NfcNdefEvt must fit the medium pool");
_Static_assert(sizeof(NfcInfoEvt) <= KQP_BLOCK_MEDIUM,
               "NfcInfoEvt must fit the medium pool");
/* The console's answer carries KQP_NFC_TEXT_MAX of message on top of the
 * tag, which is what puts it above the medium tier. Large is where it
 * belongs and where QF will serve it from; the assert is here so that
 * widening the text is a compile error rather than a Q_NEW_X that starts
 * returning NULL on a board. */
_Static_assert(sizeof(NfcNdefRspEvt) <= KQP_BLOCK_LARGE,
               "NfcNdefRspEvt must fit the large pool");
_Static_assert(sizeof(LedBlinkEvt) <= KQP_BLOCK_SMALL,
               "LedBlinkEvt must fit the small pool");
_Static_assert(sizeof(LedRgbEvt) <= KQP_BLOCK_SMALL,
               "LedRgbEvt must fit the small pool");
_Static_assert(sizeof(AppProgressEvt) <= KQP_BLOCK_MEDIUM,
               "AppProgressEvt must fit the medium pool");
_Static_assert(sizeof(AppFlashDoneEvt) <= KQP_BLOCK_MEDIUM,
               "AppFlashDoneEvt must fit the medium pool");
_Static_assert(sizeof(BrightnessEvt) <= KQP_BLOCK_SMALL,
               "BrightnessEvt must fit the small pool");
_Static_assert(sizeof(ConsoleByteEvt) <= KQP_BLOCK_SMALL,
               "ConsoleByteEvt must fit the small pool");
_Static_assert(sizeof(MqttMsgEvt) <= KQP_BLOCK_LARGE,
               "MqttMsgEvt must fit the large pool");
#if CONFIG_KODE_SCHEDULER
_Static_assert(sizeof(SchedSetReqEvt) <= KQP_BLOCK_LARGE,
               "SchedSetReqEvt must fit the large pool");
_Static_assert(sizeof(SchedEntryEvt) <= KQP_BLOCK_LARGE,
               "SchedEntryEvt must fit the large pool");
_Static_assert(sizeof(SchedSetAoReqEvt) <= KQP_BLOCK_MEDIUM,
               "SchedSetAoReqEvt must fit the medium pool");
_Static_assert(sizeof(SchedNameReqEvt) <= KQP_BLOCK_MEDIUM,
               "SchedNameReqEvt must fit the medium pool");
_Static_assert(sizeof(SchedEndEvt) <= KQP_BLOCK_MEDIUM,
               "SchedEndEvt must fit the medium pool");
_Static_assert(sizeof(SchedFireEvt) <= KQP_BLOCK_MEDIUM,
               "SchedFireEvt must fit the medium pool");
_Static_assert(sizeof(SchedCheckDoneEvt) <= KQP_BLOCK_SMALL,
               "SchedCheckDoneEvt must fit the small pool");
#endif
_Static_assert(sizeof(NetCheckAtEvt) <= KQP_BLOCK_SMALL,
               "NetCheckAtEvt must fit the small pool");
_Static_assert(sizeof(LogLevelEvt) <= KQP_BLOCK_MEDIUM,
               "LogLevelEvt must fit the medium pool");
/* This one carries a second job: a component ABOVE the SDK that posts a
 * private event of its own - kgui's SheetOpenEvt, a QEvt and a byte - cannot
 * be named here, and its own check is written against ButtonEvt instead. So
 * this line is what puts that event in a pool, and narrowing ButtonEvt would
 * move more than the buttons. */
_Static_assert(sizeof(ButtonEvt) <= KQP_BLOCK_SMALL,
               "ButtonEvt must fit the small pool");
_Static_assert(sizeof(ButtonHoldEvt) <= KQP_BLOCK_SMALL,
               "ButtonHoldEvt must fit the small pool");
_Static_assert(sizeof(ButtonDoubleEvt) <= KQP_BLOCK_SMALL,
               "ButtonDoubleEvt must fit the small pool");
_Static_assert(sizeof(IoexpInputEvt) <= KQP_BLOCK_SMALL,
               "IoexpInputEvt must fit the small pool");
_Static_assert(sizeof(ButtonsTestEvt) <= KQP_BLOCK_SMALL,
               "ButtonsTestEvt must fit the small pool");
_Static_assert(sizeof(TestReqEvt) <= KQP_BLOCK_SMALL,
               "TestReqEvt must fit the small pool");
_Static_assert(sizeof(IoexpFieldReqEvt) <= KQP_BLOCK_MEDIUM,
               "IoexpFieldReqEvt must fit the medium pool");
_Static_assert(sizeof(IoexpFieldEvt) <= KQP_BLOCK_MEDIUM,
               "IoexpFieldEvt must fit the medium pool");
_Static_assert(sizeof(PowerEvt) <= KQP_BLOCK_MEDIUM,
               "PowerEvt must fit the medium pool");
_Static_assert(sizeof(PowerFieldReqEvt) <= KQP_BLOCK_MEDIUM,
               "PowerFieldReqEvt must fit the medium pool");
_Static_assert(sizeof(PowerFieldEvt) <= KQP_BLOCK_MEDIUM,
               "PowerFieldEvt must fit the medium pool");
_Static_assert(sizeof(PowerTestReqEvt) <= KQP_BLOCK_SMALL,
               "PowerTestReqEvt must fit the small pool");
_Static_assert(sizeof(StorageCmdEvt) <= KQP_BLOCK_SMALL,
               "StorageCmdEvt must fit the small pool");
_Static_assert(sizeof(StorageBufEvt) <= KQP_BLOCK_SMALL,
               "StorageBufEvt must fit the small pool");
_Static_assert(sizeof(StorageDataEvt) <= KQP_BLOCK_SMALL,
               "StorageDataEvt must fit the small pool");
_Static_assert(sizeof(StorageOpenRspEvt) <= KQP_BLOCK_SMALL,
               "StorageOpenRspEvt must fit the small pool");
_Static_assert(sizeof(FlushEvt) <= KQP_BLOCK_MEDIUM,
               "FlushEvt must fit the medium pool");
_Static_assert(sizeof(AppFlashEvt) <= KQP_BLOCK_MEDIUM,
               "AppFlashEvt must fit the medium pool");
_Static_assert(sizeof(AppCatReqEvt) <= KQP_BLOCK_MEDIUM,
               "AppCatReqEvt must fit the medium pool");
_Static_assert(sizeof(AppCatRspEvt) <= KQP_BLOCK_SMALL,
               "AppCatRspEvt must fit the small pool");
_Static_assert(sizeof(GuiStateEvt) <= KQP_BLOCK_MEDIUM,
               "GuiStateEvt must fit the medium pool");
_Static_assert(sizeof(FunctionAnsEvt) <= KQP_BLOCK_MEDIUM,
               "FunctionAnsEvt must fit the medium pool");
_Static_assert(sizeof(StorageEntryEvt) <= KQP_BLOCK_MEDIUM,
               "StorageEntryEvt must fit the medium pool");
_Static_assert(sizeof(StorageOpenEvt) <= KQP_BLOCK_MEDIUM,
               "StorageOpenEvt must fit the medium pool");
_Static_assert(sizeof(TestResultEvt) <= KQP_BLOCK_MEDIUM,
               "TestResultEvt must fit the medium pool");
/* BootRunEvt is a QEvt and three words, as wide as the small tier on the P4
 * (20 bytes), so QF serves it from the small pool; this line only keeps it
 * inside the medium tier, where it goes if it ever grows. One per launch. */
_Static_assert(sizeof(BootRunEvt) <= KQP_BLOCK_MEDIUM,
               "BootRunEvt must fit the medium pool");
_Static_assert(sizeof(KodeScreenReqEvt) <= KQP_BLOCK_MEDIUM,
               "KodeScreenReqEvt must fit the medium pool");
_Static_assert(sizeof(AppEntryEvt) <= KQP_BLOCK_MEDIUM,
               "AppEntryEvt must fit the medium pool");
_Static_assert(sizeof(AppListReqEvt) <= KQP_BLOCK_SMALL,
               "AppListReqEvt must fit the small pool");
_Static_assert(sizeof(StorageListPageEvt) <= KQP_BLOCK_SMALL,
               "StorageListPageEvt must fit the small pool");
_Static_assert(sizeof(ListEndEvt) <= KQP_BLOCK_SMALL,
               "ListEndEvt must fit the small pool");
_Static_assert(sizeof(AppEvictedEvt) <= KQP_BLOCK_MEDIUM,
               "AppEvictedEvt must fit the medium pool");
_Static_assert(sizeof(AppEvictReqEvt) <= KQP_BLOCK_MEDIUM,
               "AppEvictReqEvt must fit the medium pool");
_Static_assert(sizeof(AppEvictEntryEvt) <= KQP_BLOCK_MEDIUM,
               "AppEvictEntryEvt must fit the medium pool");
_Static_assert(sizeof(AppEvictEndEvt) <= KQP_BLOCK_SMALL,
               "AppEvictEndEvt must fit the small pool");
_Static_assert(sizeof(ConsoleRawRspEvt) <= KQP_BLOCK_SMALL,
               "ConsoleRawRspEvt must fit the small pool");
_Static_assert(sizeof(AppRecvReqEvt) <= KQP_BLOCK_MEDIUM,
               "AppRecvReqEvt must fit the medium pool");
_Static_assert(sizeof(ConsoleRawReqEvt) <= KQP_BLOCK_MEDIUM,
               "ConsoleRawReqEvt must fit the medium pool");
_Static_assert(sizeof(StorageFieldReqEvt) <= KQP_BLOCK_MEDIUM,
               "StorageFieldReqEvt must fit the medium pool");
_Static_assert(sizeof(StorageFieldEvt) <= KQP_BLOCK_MEDIUM,
               "StorageFieldEvt must fit the medium pool");
_Static_assert(sizeof(StorageTestEvt) <= KQP_BLOCK_MEDIUM,
               "StorageTestEvt must fit the medium pool");
_Static_assert(sizeof(StorageDescReqEvt) <= KQP_BLOCK_MEDIUM,
               "StorageDescReqEvt must fit the medium pool");
_Static_assert(sizeof(StorageDescRspEvt) <= KQP_BLOCK_MEDIUM,
               "StorageDescRspEvt must fit the medium pool");
/* AppDataReqEvt is 32 + a pointer + QEvt - too wide for the small pool
 * (TouchEvt's width), so it lands in the medium one instead. */
_Static_assert(sizeof(AppDataReqEvt) <= KQP_BLOCK_MEDIUM,
               "AppDataReqEvt must fit the medium pool");
_Static_assert(sizeof(AppDataFieldsEvt) <= KQP_BLOCK_MEDIUM,
               "AppDataFieldsEvt must fit the medium pool");
_Static_assert(sizeof(AppDataIconEvt) <= KQP_BLOCK_JUMBO,
               "AppDataIconEvt must fit the jumbo pool");
_Static_assert(sizeof(AppDataRmRspEvt) <= KQP_BLOCK_MEDIUM,
               "AppDataRmRspEvt must fit the medium pool");
_Static_assert(sizeof(RecvDataEvt) <= KQP_BLOCK_JUMBO,
               "RecvDataEvt must fit the jumbo pool");
_Static_assert(sizeof(StoragePathEvt) <= KQP_BLOCK_MEDIUM,
               "StoragePathEvt must fit the medium pool");
_Static_assert(sizeof(StoragePathRspEvt) <= KQP_BLOCK_SMALL,
               "StoragePathRspEvt must fit the small pool");
_Static_assert(sizeof(StorageLineEvt) <= KQP_BLOCK_MEDIUM,
               "StorageLineEvt must fit the medium pool");
_Static_assert(sizeof(StorageTextEvt) <= KQP_BLOCK_MEDIUM,
               "StorageTextEvt must fit the medium pool");
/* The event KODE_PATH_MAX was sized against: two whole paths in one block is
 * the tightest thing the medium tier carries, so if any of these checks is
 * load-bearing it is this one. */
_Static_assert(sizeof(StoragePairEvt) <= KQP_BLOCK_MEDIUM,
               "StoragePairEvt must fit the medium pool");
/* The answers kode_sys's file and network requests get, and the network's asks. */
_Static_assert(sizeof(FileRspEvt) <= KQP_BLOCK_MEDIUM,
               "FileRspEvt must fit the medium pool");
_Static_assert(sizeof(NetHostRspEvt) <= KQP_BLOCK_MEDIUM,
               "NetHostRspEvt must fit the medium pool");
_Static_assert(sizeof(NetHttpRspEvt) <= KQP_BLOCK_MEDIUM,
               "NetHttpRspEvt must fit the medium pool");
_Static_assert(sizeof(NetHostAskEvt) <= KQP_BLOCK_MEDIUM,
               "NetHostAskEvt must fit the medium pool");
_Static_assert(sizeof(NetHttpAskEvt) <= KQP_BLOCK_MEDIUM,
               "NetHttpAskEvt must fit the medium pool");
_Static_assert(sizeof(NetUdpAskEvt) <= KQP_BLOCK_MEDIUM,
               "NetUdpAskEvt must fit the medium pool");
_Static_assert(sizeof(StorageRecvEvt) <= KQP_BLOCK_MEDIUM,
               "StorageRecvEvt must fit the medium pool");
_Static_assert(sizeof(MirrorStartEvt) <= KQP_BLOCK_SMALL,
               "MirrorStartEvt must fit the small pool");
_Static_assert(sizeof(MirrorAckEvt) <= KQP_BLOCK_SMALL,
               "MirrorAckEvt must fit the small pool");

/* AO_Net's events, checked as a block for one property: none of them may
 * reach the jumbo pool. That pool has four blocks and exists for two things:
 * a piece of an arriving firmware image, and a third of an app icon leaving
 * a medium; nothing else may reach it - so an event that quietly grew past
 * the large tier would starve a terminal install (or `app_data`) and be
 * discovered during a transfer. Which tier below that they land in does not
 * matter; QF serves from the smallest that fits. */
_Static_assert(sizeof(NetPingReqEvt) <= KQP_BLOCK_LARGE,
               "NetPingReqEvt must not reach the jumbo pool");
_Static_assert(sizeof(NetPingEntryEvt) <= KQP_BLOCK_MEDIUM,
               "NetPingEntryEvt must fit the medium pool");
_Static_assert(sizeof(NetPingEndEvt) <= KQP_BLOCK_MEDIUM,
               "NetPingEndEvt must fit the medium pool");
_Static_assert(sizeof(NetHttpReqEvt) <= KQP_BLOCK_LARGE,
               "NetHttpReqEvt must not reach the jumbo pool");
_Static_assert(sizeof(NetHttpDataEvt) <= KQP_BLOCK_LARGE,
               "NetHttpDataEvt must fit the large pool");
_Static_assert(sizeof(NetHttpDoneEvt) <= KQP_BLOCK_MEDIUM,
               "NetHttpDoneEvt must fit the medium pool");
_Static_assert(sizeof(NetUdpReqEvt) <= KQP_BLOCK_LARGE,
               "NetUdpReqEvt must not reach the jumbo pool");
_Static_assert(sizeof(NetUdpDoneEvt) <= KQP_BLOCK_SMALL,
               "NetUdpDoneEvt must fit the small pool");
_Static_assert(sizeof(NetInfoEvt) <= KQP_BLOCK_MEDIUM,
               "NetInfoEvt must fit the medium pool");
_Static_assert(sizeof(NetMqttLinkEvt) <= KQP_BLOCK_SMALL,
               "NetMqttLinkEvt must fit the small pool");
_Static_assert(sizeof(NetLineDoneEvt) <= KQP_BLOCK_SMALL,
               "NetLineDoneEvt must fit the small pool");
_Static_assert(sizeof(MqttUpEvt) <= KQP_BLOCK_LARGE,
               "MqttUpEvt must not reach the jumbo pool");
_Static_assert(sizeof(MqttTopicEvt) <= KQP_BLOCK_MEDIUM,
               "MqttTopicEvt must fit the medium pool");
_Static_assert(sizeof(MqttPubEvt) <= KQP_BLOCK_LARGE,
               "MqttPubEvt must not reach the jumbo pool");
_Static_assert(sizeof(MqttStateEvt) <= KQP_BLOCK_LARGE,
               "MqttStateEvt must not reach the jumbo pool");
_Static_assert(sizeof(MqttMsgEvt) <= KQP_BLOCK_LARGE,
               "MqttMsgEvt must not reach the jumbo pool");

/* Everything else that is allocated from a pool, checked for the one property
 * the block above states for AO_Net: the jumbo pool has four blocks and exists
 * for two things, a piece of an arriving firmware image and a third of an app
 * icon leaving a medium, so nothing else may reach it. Which tier below that
 * an event lands in does not matter - QF serves from the smallest that fits.
 *
 * These eighteen had no guard at all until now, which is why they are worth
 * listing rather than trusting: none of them is wrong today, and the guard is
 * what keeps that true. FlashReqEvt is the one to watch - it has the least
 * headroom of the group, and KODE_FLASH_SRC_MAX is the number that would
 * spend it.
 *
 * They belong here rather than in the host suite on purpose: the bench is
 * LP64 and the board ILP32, and several of these carry a pointer, so only a
 * firmware build measures the struct the board actually ships. */
_Static_assert(sizeof(FlashReqEvt) <= KQP_BLOCK_LARGE,
               "FlashReqEvt must not reach the jumbo pool");
_Static_assert(sizeof(NetStreamOpenEvt) <= KQP_BLOCK_LARGE,
               "NetStreamOpenEvt must not reach the jumbo pool");
_Static_assert(sizeof(StorageWriteEvt) <= KQP_BLOCK_LARGE,
               "StorageWriteEvt must not reach the jumbo pool");
_Static_assert(sizeof(StorageWriteRspEvt) <= KQP_BLOCK_LARGE,
               "StorageWriteRspEvt must not reach the jumbo pool");
_Static_assert(sizeof(StorageInfoEvt) <= KQP_BLOCK_LARGE,
               "StorageInfoEvt must not reach the jumbo pool");
_Static_assert(sizeof(RadioStateEvt) <= KQP_BLOCK_LARGE,
               "RadioStateEvt must not reach the jumbo pool");
_Static_assert(sizeof(RadioLostEvt) <= KQP_BLOCK_SMALL,
               "RadioLostEvt must fit the small pool");
_Static_assert(sizeof(KodeSvcEvt) <= KQP_BLOCK_SMALL,
               "KodeSvcEvt must fit the small pool");
_Static_assert(sizeof(WifiJoinEvt) <= KQP_BLOCK_LARGE,
               "WifiJoinEvt must not reach the jumbo pool");
_Static_assert(sizeof(WifiScanEntryEvt) <= KQP_BLOCK_LARGE,
               "WifiScanEntryEvt must not reach the jumbo pool");
_Static_assert(sizeof(WifiScanEndEvt) <= KQP_BLOCK_LARGE,
               "WifiScanEndEvt must not reach the jumbo pool");
_Static_assert(sizeof(WifiSavedEvt) <= KQP_BLOCK_LARGE,
               "WifiSavedEvt must not reach the jumbo pool");
_Static_assert(sizeof(WifiStateEvt) <= KQP_BLOCK_LARGE,
               "WifiStateEvt must not reach the jumbo pool");
_Static_assert(sizeof(DisplayStateEvt) <= KQP_BLOCK_LARGE,
               "DisplayStateEvt must not reach the jumbo pool");
_Static_assert(sizeof(DisplayCaptureReqEvt) <= KQP_BLOCK_LARGE,
               "DisplayCaptureReqEvt must not reach the jumbo pool");
_Static_assert(sizeof(DisplayCaptureRspEvt) <= KQP_BLOCK_LARGE,
               "DisplayCaptureRspEvt must not reach the jumbo pool");
_Static_assert(sizeof(DisplayShotReqEvt) <= KQP_BLOCK_SMALL,
               "DisplayShotReqEvt must fit the small pool");
_Static_assert(sizeof(DisplayShotRspEvt) <= KQP_BLOCK_SMALL,
               "DisplayShotRspEvt must fit the small pool");
_Static_assert(sizeof(NetShotReqEvt) <= KQP_BLOCK_MEDIUM,
               "NetShotReqEvt must fit the medium pool");
_Static_assert(sizeof(NetShotRspEvt) <= KQP_BLOCK_MEDIUM,
               "NetShotRspEvt must fit the medium pool");
_Static_assert(sizeof(NetShotStopEvt) <= KQP_BLOCK_SMALL,
               "NetShotStopEvt must fit the small pool");
_Static_assert(sizeof(ButtonsStateEvt) <= KQP_BLOCK_LARGE,
               "ButtonsStateEvt must not reach the jumbo pool");
_Static_assert(sizeof(BootStateEvt) <= KQP_BLOCK_LARGE,
               "BootStateEvt must not reach the jumbo pool");
_Static_assert(sizeof(BleScanReqEvt) <= KQP_BLOCK_SMALL,
               "BleScanReqEvt is one integer: the small pool");
_Static_assert(sizeof(BleScanEvt) <= KQP_BLOCK_LARGE,
               "BleScanEvt must not reach the jumbo pool");
_Static_assert(sizeof(IrRxEvt) <= KQP_BLOCK_LARGE,
               "IrRxEvt must not reach the jumbo pool");
_Static_assert(sizeof(IrCaptureEvt) <= KQP_BLOCK_LARGE,
               "IrCaptureEvt must not reach the jumbo pool");
_Static_assert(sizeof(IrSendRawEvt) <= KQP_BLOCK_MEDIUM,
               "IrSendRawEvt must fit the medium pool");
_Static_assert(sizeof(IrLastEvt) <= KQP_BLOCK_LARGE,
               "IrLastEvt must not reach the jumbo pool");
/* The answers kode_sys's infrared, NFC and RFID requests get: a status and a copy. */
_Static_assert(sizeof(StatusRspEvt) <= KQP_BLOCK_SMALL,
               "StatusRspEvt must fit the small pool");
_Static_assert(sizeof(IrCaptureRspEvt) <= KQP_BLOCK_SMALL,
               "IrCaptureRspEvt must fit the small pool");
_Static_assert(sizeof(IrLastRspEvt) <= KQP_BLOCK_SMALL,
               "IrLastRspEvt must fit the small pool");
_Static_assert(sizeof(NfcReadRspEvt) <= KQP_BLOCK_MEDIUM,
               "NfcReadRspEvt must fit the medium pool");
_Static_assert(sizeof(RfidEmuRspEvt) <= KQP_BLOCK_SMALL,
               "RfidEmuRspEvt must fit the small pool");
/* An emulation request carries its card by value, which puts it in the large tier. */
_Static_assert(sizeof(NfcEmuReqEvt) <= KQP_BLOCK_LARGE,
               "NfcEmuReqEvt must not reach the jumbo pool");
_Static_assert(sizeof(RfidWriteReqEvt) <= KQP_BLOCK_MEDIUM,
               "RfidWriteReqEvt must fit the medium pool");
_Static_assert(sizeof(IrInfoReqEvt) <= KQP_BLOCK_LARGE,
               "IrInfoReqEvt must not reach the jumbo pool");
_Static_assert(sizeof(IrInfoEvt) <= KQP_BLOCK_LARGE,
               "IrInfoEvt must not reach the jumbo pool");
/* Tighter than the line above it, because IrSendEvt's own documentation in
 * kqp_signals.h states the tier: two uint16_t fields chosen so a transmit
 * request lands in the small pool rather than the jumbo one a symbol array
 * would have forced. A claim a header makes about a size belongs here. */
_Static_assert(sizeof(IrSendEvt) <= KQP_BLOCK_SMALL,
               "IrSendEvt must fit the small pool");

/* The audio row's records. AudioTestEvt sits in the medium pool beside
 * StorageTestEvt, which is the same kind of thing: one phase of a long row,
 * carrying what it measured. It is eleven scalars and no more, because every
 * level on this path is a tenth-decibel integer rather than a float - the
 * event, the log line and the console record all carry the same number the
 * detector produced. */
_Static_assert(sizeof(AudioTestEvt) <= KQP_BLOCK_MEDIUM,
               "AudioTestEvt must fit the medium pool");
_Static_assert(sizeof(AudioToneEvt) <= KQP_BLOCK_SMALL,
               "AudioToneEvt must fit the small pool");
/* Beside HapticDoneEvt, which it is the audio half of: a tag and an
 * outcome, and nothing an Active Object would have had to sleep for. */
_Static_assert(sizeof(AudioDoneEvt) <= KQP_BLOCK_SMALL,
               "AudioDoneEvt must fit the small pool");
_Static_assert(sizeof(AudioFieldReqEvt) <= KQP_BLOCK_SMALL,
               "AudioFieldReqEvt must fit the small pool");
/* Beside PowerFieldEvt, which it is a copy of in everything but the
 * chip it names. */
_Static_assert(sizeof(AudioFieldEvt) <= KQP_BLOCK_MEDIUM,
               "AudioFieldEvt must fit the medium pool");
_Static_assert(sizeof(AudioBufEvt) <= KQP_BLOCK_LARGE,
               "AudioBufEvt must not reach the jumbo pool");
_Static_assert(sizeof(AudioInfoReqEvt) <= KQP_BLOCK_LARGE,
               "AudioInfoReqEvt must not reach the jumbo pool");
/* A path is 96 bytes, so this one is the widest of the audio events and
 * belongs beside the other events that carry a name. */
_Static_assert(sizeof(AudioFileReqEvt) <= KQP_BLOCK_LARGE,
               "AudioFileReqEvt must not reach the jumbo pool");
_Static_assert(sizeof(AudioFileRspEvt) <= KQP_BLOCK_LARGE,
               "AudioFileRspEvt must not reach the jumbo pool");
_Static_assert(sizeof(AudioInfoRspEvt) <= KQP_BLOCK_MEDIUM,
               "AudioInfoRspEvt must fit the medium pool");

/* The IMU's and the haptic's traffic. Nothing here carries a name or a
 * buffer, so the widest of them is the four-scalar row record: the whole
 * family sits in the small and medium tiers, and the asserts say which
 * because a field added later is exactly how a struct crosses a tier
 * without anybody noticing. */
_Static_assert(sizeof(ImuCaptureReqEvt) <= KQP_BLOCK_SMALL,
               "ImuCaptureReqEvt must fit the small pool");
_Static_assert(sizeof(ImuArmedEvt) <= KQP_BLOCK_SMALL,
               "ImuArmedEvt must fit the small pool");
_Static_assert(sizeof(ImuDrainReqEvt) <= KQP_BLOCK_SMALL,
               "ImuDrainReqEvt must fit the small pool");
_Static_assert(sizeof(HapticFieldReqEvt) <= KQP_BLOCK_SMALL,
               "HapticFieldReqEvt must fit the small pool");
/* Beside AudioInfoReqEvt, which it is a copy of but for the one extra
 * pointer the IMU's read carries. */
_Static_assert(sizeof(ImuReadEvt) <= KQP_BLOCK_MEDIUM,
               "ImuReadEvt must fit the medium pool");
/* The answers kode_sys's motion requests get: a tag, a status and a copy. */
_Static_assert(sizeof(ImuSampleRspEvt) <= KQP_BLOCK_MEDIUM,
               "ImuSampleRspEvt must fit the medium pool");
_Static_assert(sizeof(MagSampleRspEvt) <= KQP_BLOCK_MEDIUM,
               "MagSampleRspEvt must fit the medium pool");
_Static_assert(sizeof(MagCalRspEvt) <= KQP_BLOCK_MEDIUM,
               "MagCalRspEvt must fit the medium pool");
/* Sixteen segments of two bytes put the pattern past the small tier. */
_Static_assert(sizeof(HapticPlayEvt) <= KQP_BLOCK_MEDIUM,
               "HapticPlayEvt must fit the medium pool");
_Static_assert(sizeof(ImuEnergyEvt) <= KQP_BLOCK_MEDIUM,
               "ImuEnergyEvt must fit the medium pool");
/* Beside AudioFieldEvt, which it is a copy of in everything but the chip it
 * names. */
_Static_assert(sizeof(HapticFieldEvt) <= KQP_BLOCK_MEDIUM,
               "HapticFieldEvt must fit the medium pool");
_Static_assert(sizeof(HapticDoneEvt) <= KQP_BLOCK_MEDIUM,
               "HapticDoneEvt must fit the medium pool");
/* Beside AudioTestEvt and StorageTestEvt, which are the same kind of thing:
 * one step of a long row, carrying what it measured. */
_Static_assert(sizeof(ImuTestEvt) <= KQP_BLOCK_MEDIUM,
               "ImuTestEvt must fit the medium pool");
_Static_assert(sizeof(HapticTestEvt) <= KQP_BLOCK_MEDIUM,
               "HapticTestEvt must fit the medium pool");

/* The two rows an operator judges: a question or its outcome, and nothing
 * measured to carry. */
_Static_assert(sizeof(DisplayTestEvt) <= KQP_BLOCK_SMALL,
               "DisplayTestEvt must fit the small pool");
_Static_assert(sizeof(LedTestEvt) <= KQP_BLOCK_SMALL,
               "LedTestEvt must fit the small pool");

/* A worker's done event is posted with the pool block already in hand, so a
 * block it cannot fit would never be posted at all. */
_Static_assert(sizeof(KqpWorkerDoneEvt) <= KQP_BLOCK_SMALL,
               "KqpWorkerDoneEvt must fit the small pool");

void kqp_init(void)
{
    QF_init();

    QF_psInit(s_subscr_sto, Q_DIM(s_subscr_sto));

    QF_poolInit(s_small_pool,  sizeof(s_small_pool),  sizeof(s_small_pool[0]));
    QF_poolInit(s_medium_pool, sizeof(s_medium_pool), sizeof(s_medium_pool[0]));
    QF_poolInit(s_large_pool,  sizeof(s_large_pool),  sizeof(s_large_pool[0]));
    QF_poolInit(s_jumbo_pool,  sizeof(s_jumbo_pool),  sizeof(s_jumbo_pool[0]));

    /* Last: a framework with a clock but no pools would hand a QTimeEvt to an
     * AO that cannot allocate. */
    kqp_tick_start();

#if CONFIG_KQP_AO_WATCH
    /* After the tick: the supervisor reads Active Objects that exist by now,
     * and the ones started later appear in its next pass. */
    kqp_watch_start();
#endif
}

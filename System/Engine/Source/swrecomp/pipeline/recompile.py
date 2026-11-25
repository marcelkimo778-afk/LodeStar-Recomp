"""Link a program: assign module bases, apply relocations, resolve imports,
emit recompiled C and the runtime program tables.

This is the static linker stage of the pipeline.  It runs entirely ahead of time:
the standalone executable never parses an NSO nor applies a relocation.
"""

from __future__ import annotations

import json
import os
import struct
from dataclasses import dataclass, field
from pathlib import Path

from ..arm64.emit import emit_function, set_function_index
from ..arm64.ir import Function
from ..arm64.lift import Lifter
from ..errors import UnsupportedError
from ..formats.nso import Nso, Symbol

R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027

THUNK_BASE = 0xFF000000

# Imports implemented natively by the compatibility runtime.  Everything else
# becomes an explicit trap stub that names the missing symbol when it is hit.
NATIVE_SERVICES: dict[str, str] = {
    "__nnmusl_init_dso": "svc_noop",
    "__nnmusl_fini_dso": "svc_noop",
    "__nnmusl_init_dso_array": "svc_noop",
    "__cxa_guard_acquire": "svc_cxa_guard_acquire",
    "__cxa_guard_release": "svc_cxa_guard_release",
    "__cxa_guard_abort": "svc_cxa_guard_release",
    "__cxa_atexit": "svc_noop",
    "__cxa_finalize": "svc_noop",
    "__cxa_pure_virtual": "svc_abort",
    "malloc": "svc_malloc",
    "calloc": "svc_calloc",
    "realloc": "svc_realloc",
    "free": "svc_free",
    "memcpy": "svc_memcpy",
    "memmove": "svc_memmove",
    "memset": "svc_memset",
    "memcmp": "svc_memcmp",
    "strlen": "svc_strlen",
    "strcmp": "svc_strcmp",
    "strncmp": "svc_strncmp",
    "strcpy": "svc_strcpy",
    "strncpy": "svc_strncpy",
    "abort": "svc_abort",
    "puts": "svc_noop",
    "printf": "svc_noop",

    # nn::os - Threading
        "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi": "svc_nn_os_CreateThread",
        "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii": "svc_nn_os_CreateThread",
        "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE": "svc_nn_os_StartThread",
        "_ZN2nn2os8WaitThreadEPNS0_10ThreadTypeE": "svc_nn_os_WaitThread",
        "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE": "svc_nn_os_WaitThread",
        "_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE": "svc_nn_os_DestroyThread",
        "_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc": "svc_nn_os_SetThreadName",
        "_ZN2nn2os20SetThreadNamePointerEPNS0_10ThreadTypeEPKc": "svc_nn_os_SetThreadName",
        "_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim": "svc_nn_os_SetThreadCoreMask",
        "_ZN2nn2os17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE": "svc_nn_os_GetThreadCoreMask",
        "_ZN2nn2os26GetThreadAvailableCoreMaskEv": "svc_nn_os_GetThreadAvailableCoreMask",
        "_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE": "svc_nn_os_GetThreadPriority",
        "_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi": "svc_nn_os_ChangeThreadPriority",
        "_ZN2nn2os11SleepThreadENS_8TimeSpanE": "svc_nn_os_SleepThread",
        "_ZN2nn2os11YieldThreadEv": "svc_nn_os_YieldThread",
        "_ZN2nn2os16GetCurrentThreadEv": "svc_nn_os_GetCurrentThread",
        "_ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE": "svc_nn_os_GetThreadId",
        "_ZN2nn2os20GetCurrentCoreNumberEv": "svc_nn_os_GetCurrentCoreNumber",

        # nn::os - TLS
        "_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE": "svc_nn_os_AllocateTlsSlot",
        "_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE": "svc_nn_os_FreeTlsSlot",
        "_ZN2nn2os11GetTlsValueENS0_7TlsSlotE": "svc_nn_os_GetTlsValue",
        "_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm": "svc_nn_os_SetTlsValue",
        "_ZN2nn2os18SdkAllocateTlsSlotEPNS0_7TlsSlotEPFvmE": "svc_nn_os_SdkAllocateTlsSlot",

        # nn::os - Mutex
        "_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi": "svc_nn_os_InitializeMutex",
        "_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE": "svc_nn_os_FinalizeMutex",
        "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE": "svc_nn_os_LockMutex",
        "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE": "svc_nn_os_UnlockMutex",
        "_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE": "svc_nn_os_TryLockMutex",
        "_ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE": "svc_nn_os_IsMutexLockedByCurrentThread",
        "_ZN2nn2os12SdkMutexType10InitializeEv": "svc_nn_os_SdkMutexType_Initialize",
        "_ZN2nn2os12SdkMutexType4LockEv": "svc_nn_os_SdkMutexType_Lock",
        "_ZN2nn2os12SdkMutexType6UnlockEv": "svc_nn_os_SdkMutexType_Unlock",
        "_ZN2nn2os21SdkRecursiveMutexType10InitializeEv": "svc_nn_os_SdkRecursiveMutexType_Initialize",
        "_ZN2nn2os21SdkRecursiveMutexType4LockEv": "svc_nn_os_SdkRecursiveMutexType_Lock",
        "_ZN2nn2os21SdkRecursiveMutexType6UnlockEv": "svc_nn_os_SdkRecursiveMutexType_Unlock",

        # nn::os - Condition Variable
        "_ZN2nn2os27InitializeConditionVariableEPNS0_21ConditionVariableTypeE": "svc_nn_os_InitializeConditionVariable",
        "_ZN2nn2os25FinalizeConditionVariableEPNS0_21ConditionVariableTypeE": "svc_nn_os_FinalizeConditionVariable",
        "_ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE": "svc_nn_os_WaitConditionVariable",
        "_ZN2nn2os26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE": "svc_nn_os_TimedWaitConditionVariable",
        "_ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE": "svc_nn_os_SignalConditionVariable",
        "_ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE": "svc_nn_os_BroadcastConditionVariable",

        # nn::os - Event
        "_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE": "svc_nn_os_InitializeEvent",
        "_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE": "svc_nn_os_FinalizeEvent",
        "_ZN2nn2os11SignalEventEPNS0_9EventTypeE": "svc_nn_os_SignalEvent",
        "_ZN2nn2os10ClearEventEPNS0_9EventTypeE": "svc_nn_os_ClearEvent",
        "_ZN2nn2os9WaitEventEPNS0_9EventTypeE": "svc_nn_os_WaitEvent",
        "_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE": "svc_nn_os_TryWaitEvent",
        "_ZN2nn2os14TimedWaitEventEPNS0_9EventTypeENS_8TimeSpanE": "svc_nn_os_TimedWaitEvent",
        "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE": "svc_nn_os_InitializeLightEvent",
        "_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE": "svc_nn_os_FinalizeLightEvent",
        "_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE": "svc_nn_os_SignalLightEvent",
        "_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE": "svc_nn_os_WaitLightEvent",
        "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE": "svc_nn_os_DestroySystemEvent",
        "_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE": "svc_nn_os_WaitSystemEvent",
        "_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE": "svc_nn_os_TryWaitSystemEvent",
        "_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE": "svc_nn_os_ClearSystemEvent",
        "_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE": "svc_nn_os_AttachReadableHandleToSystemEvent",

        # nn::os - Fiber
        "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi": "svc_nn_os_InitializeFiber",
        "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE": "svc_nn_os_FinalizeFiber",
        "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE": "svc_nn_os_SwitchToFiber",
        "_ZN2nn2os15GetCurrentFiberEv": "svc_nn_os_GetCurrentFiber",

        # nn::os - Message Queue
        "_ZN2nn2os22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm": "svc_nn_os_InitializeMessageQueue",
        "_ZN2nn2os20FinalizeMessageQueueEPNS0_16MessageQueueTypeE": "svc_nn_os_FinalizeMessageQueue",
        "_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm": "svc_nn_os_SendMessageQueue",
        "_ZN2nn2os19TrySendMessageQueueEPNS0_16MessageQueueTypeEm": "svc_nn_os_TrySendMessageQueue",
        "_ZN2nn2os19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE": "svc_nn_os_ReceiveMessageQueue",
        "_ZN2nn2os22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE": "svc_nn_os_TryReceiveMessageQueue",
        "_ZN2nn2os15JamMessageQueueEPNS0_16MessageQueueTypeEm": "svc_nn_os_JamMessageQueue",
        "_ZN2nn2os18TryJamMessageQueueEPNS0_16MessageQueueTypeEm": "svc_nn_os_TryJamMessageQueue",
        "_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeE": "svc_nn_os_ReleaseSemaphore",
        "_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeEi": "svc_nn_os_ReleaseSemaphore",
        "_ZN2nn2os19InitializeSemaphoreEPNS0_13SemaphoreTypeEii": "svc_nn_os_InitializeSemaphore",
        "_ZN2nn2os17FinalizeSemaphoreEPNS0_13SemaphoreTypeE": "svc_nn_os_FinalizeSemaphore",
        "_ZN2nn2os19TryAcquireSemaphoreEPNS0_13SemaphoreTypeE": "svc_nn_os_TryAcquireSemaphore",
        "_ZN2nn2os16AcquireSemaphoreEPNS0_13SemaphoreTypeE": "svc_nn_os_AcquireSemaphore",

        # nn::os - Timer
        "_ZN2nn2os22StartOneShotTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanE": "svc_nn_os_StartOneShotTimerEvent",
        "_ZN2nn2os20InitializeTimerEventEPNS0_14TimerEventTypeENS0_14EventClearModeE": "svc_nn_os_InitializeTimerEvent",
        "_ZN2nn2os23StartPeriodicTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanES3_": "svc_nn_os_StartPeriodicTimerEvent",
        "_ZN2nn2os14WaitTimerEventEPNS0_14TimerEventTypeE": "svc_nn_os_WaitTimerEvent",
        "_ZN2nn2os14StopTimerEventEPNS0_14TimerEventTypeE": "svc_nn_os_StopTimerEvent",
        "_ZN2nn2os18FinalizeTimerEventEPNS0_14TimerEventTypeE": "svc_nn_os_FinalizeTimerEvent",

        # nn::os - Other
        "_ZN2nn2os19GetCurrentStackInfoEPmS1_": "svc_nn_os_GetCurrentStackInfo",
        "_ZN2nn2os10TryWaitAnyEPNS0_13MultiWaitTypeE": "svc_nn_os_TryWaitAny",
        "_ZN2nn2os7WaitAnyEPNS0_13MultiWaitTypeE": "svc_nn_os_WaitAny",
        "_ZN2nn2os19InitializeMultiWaitEPNS0_13MultiWaitTypeE": "svc_nn_os_InitializeMultiWait",
        "_ZN2nn2os17FinalizeMultiWaitEPNS0_13MultiWaitTypeE": "svc_nn_os_FinalizeMultiWait",
        "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE": "svc_nn_os_InitializeMultiWaitHolder",
        "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_14TimerEventTypeE": "svc_nn_os_InitializeMultiWaitHolder",
        "_ZN2nn2os23FinalizeMultiWaitHolderEPNS0_19MultiWaitHolderTypeE": "svc_nn_os_FinalizeMultiWaitHolder",
        "_ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE": "svc_nn_os_LinkMultiWaitHolder",
        "_ZN2nn2os21UnlinkMultiWaitHolderEPNS0_19MultiWaitHolderTypeE": "svc_nn_os_UnlinkMultiWaitHolder",
        "_ZN2nn2os26SetMultiWaitHolderUserDataEPNS0_19MultiWaitHolderTypeEm": "svc_nn_os_SetMultiWaitHolderUserData",
        "_ZN2nn2os26GetMultiWaitHolderUserDataEPKNS0_19MultiWaitHolderTypeE": "svc_nn_os_GetMultiWaitHolderUserData",
        "_ZN2nn2os13GetSystemTickEv": "svc_nn_os_GetSystemTick",
        "_ZN2nn2os17ConvertToTimeSpanENS0_4TickE": "svc_nn_os_ConvertToTimeSpan",
        "_ZN2nn2os22GetSystemTickFrequencyEv": "svc_nn_os_GetSystemTickFrequency",
        "_ZN2nn2os36GetVirtualAddressMemoryResourceUsageEv": "svc_nn_os_GetVirtualAddressMemoryResourceUsage",
        "_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE": "svc_nn_os_QueryMemoryInfo",
        "_ZN2nn2os17SetMemoryHeapSizeEm": "svc_nn_os_SetMemoryHeapSize",
        "_ZN2nn2os19AllocateMemoryBlockEPmm": "svc_nn_os_AllocateMemoryBlock",
        "_ZN2nn2os32SetMemoryAllocatorForThreadLocalEPFPvmmEPFvS1_mE": "svc_nn_os_SetMemoryAllocatorForThreadLocal",
        "_ZN2nn2os11GetHostArgcEv": "svc_nn_os_GetHostArgc",
        "_ZN2nn2os11GetHostArgvEv": "svc_nn_os_GetHostArgv",
        "_ZN2nn2os19GenerateRandomBytesEPvm": "svc_nn_os_GenerateRandomBytes",

        # nn::diag
        "_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultES3_z": "svc_nn_diag_AbortImpl",
        "_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i": "svc_nn_diag_AbortImpl",

        # nn::result
        "_ZN2nn6result6detail17OnUnhandledResultENS_6ResultE": "svc_nn_result_OnUnhandledResult",

        # nn::oe
        "_ZN2nn2oe18GetDesiredLanguageEv": "svc_nn_oe_GetDesiredLanguage",
        "_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE": "svc_nn_settings_GetLanguage",
        "_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE": "svc_nn_settings_SetLanguage",
        "_ZN2nn2oe17FinishStartupLogoEv": "svc_nn_oe_FinishStartupLogo",
        "_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE": "svc_nn_oe_SetFocusHandlingMode",
        "_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb": "svc_nn_oe_SetOperationModeChangedNotificationEnabled",
        "_ZN2nn2oe31EnterExitRequestHandlingSectionEv": "svc_nn_oe_EnterExitRequestHandlingSection",
        "_ZN2nn2oe31LeaveExitRequestHandlingSectionEv": "svc_nn_oe_LeaveExitRequestHandlingSection",
        "_ZN2nn2oe16GetOperationModeEv": "svc_nn_oe_GetOperationMode",
        "_ZN2nn2oe20GetCurrentFocusStateEv": "svc_nn_oe_GetCurrentFocusState",

        # nn::swkbd
        "_ZN2nn5swkbd27GetRequiredStringBufferSizeEv": "svc_nn_swkbd_GetRequiredStringBufferSize",
        "_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE": "svc_nn_swkbd_ShowKeyboard",
        "_ZN2nn5swkbd10MakePresetEPNS0_14KeyboardConfigENS0_6PresetE": "svc_nn_swkbd_MakePreset",
        "_ZN2nn5swkbd25GetRequiredWorkBufferSizeEi": "svc_nn_swkbd_GetRequiredWorkBufferSize",
        "_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb": "svc_nn_swkbd_GetRequiredWorkBufferSize",
        "_ZN2nn5swkbd15SetUserWordListEPNS0_15ShowKeyboardArgEPKNS0_8UserWordEi": "svc_nn_swkbd_SetUserWordList",
        "_ZN2nn5swkbd34GetRequiredTextCheckWorkBufferSizeEv": "svc_nn_swkbd_GetRequiredTextCheckWorkBufferSize",
        "_ZN2nn5swkbd13SetHeaderTextEPNS0_14KeyboardConfigEPKDs": "svc_nn_swkbd_SetHeaderText",
        "_ZN2nn5swkbd10SetSubTextEPNS0_14KeyboardConfigEPKDs": "svc_nn_swkbd_SetSubText",
        "_ZN2nn5swkbd20SetTextCheckCallbackEPNS0_15ShowKeyboardArgEPFNS0_15TextCheckResultEPvPmPNS0_6StringEE": "svc_nn_swkbd_SetTextCheckCallback",
        "_ZN2nn5swkbd14SetInitialTextEPNS0_15ShowKeyboardArgEPKDs": "svc_nn_swkbd_SetInitialText",

        # nn::hid
        "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj": "svc_nn_hid_GetNpadState",
        "_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj": "svc_nn_hid_GetNpadState",
        "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj": "svc_nn_hid_GetNpadState",
        "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj": "svc_nn_hid_GetNpadState",
        "_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj": "svc_nn_hid_GetNpadState",
        "_ZN2nn3hid14InitializeNpadEv": "svc_nn_hid_InitializeNpad",
        "_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE": "svc_nn_hid_SetSupportedNpadStyleSet",
        "_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm": "svc_nn_hid_SetSupportedNpadIdType",
        "_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE": "svc_nn_hid_SetNpadJoyHoldType",
        "_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE": "svc_nn_hid_SetNpadHandheldActivationMode",
        "_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE": "svc_nn_hid_BindNpadStyleSetUpdateEvent",
        "_ZN2nn3hid15GetNpadStyleSetERKj": "svc_nn_hid_GetNpadStyleSet",
        "_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE": "svc_nn_hid_GetSixAxisSensorState",
        "_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE": "svc_nn_hid_GetSixAxisSensorStates",
        "_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE": "svc_nn_hid_IsSixAxisSensorAtRest",
        "_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE": "svc_nn_hid_StartSixAxisSensor",
        "_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE": "svc_nn_hid_StopSixAxisSensor",
        "_ZN2nn3hid28IsSixAxisSensorFusionEnabledERKNS0_19SixAxisSensorHandleE": "svc_nn_hid_IsSixAxisSensorFusionEnabled",
        "_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb": "svc_nn_hid_EnableSixAxisSensorFusion",
        "_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff": "svc_nn_hid_SetSixAxisSensorFusionParameters",
        "_ZN2nn3hid32GetSixAxisSensorFusionParametersEPfS1_RKNS0_19SixAxisSensorHandleE": "svc_nn_hid_GetSixAxisSensorFusionParameters",
        "_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE": "svc_nn_hid_SetGyroscopeZeroDriftMode",
        "_ZN2nn3hid25GetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleE": "svc_nn_hid_GetGyroscopeZeroDriftMode",
        "_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE": "svc_nn_hid_GetSixAxisSensorHandles",
        "_ZN2nn3hid6NpadId8HandheldE": "svc_nn_hid_NpadIdHandheld",
        "_ZN2nn3hid6NpadId3No1E": "svc_nn_hid_NpadIdNo1",

        # nn::account
        "_ZN2nn7account10InitializeEv": "svc_nn_account_Initialize",
        "_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE": "svc_nn_account_TryOpenPreselectedUser",
        "_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE": "svc_nn_account_GetUserId",
        "_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE": "svc_nn_account_GetNickname",
        "_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE": "svc_nn_account_IsNetworkServiceAccountAvailable",
        "_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE": "svc_nn_account_GetNetworkServiceAccountId",
        "_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE": "svc_nn_account_GetUserExistence",
        "_ZN2nn7account12AsyncContextD1Ev": "svc_nn_account_AsyncContext_D1",
        "_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE": "svc_nn_account_AsyncContext_GetSystemEvent",
        "_ZN2nn7account12AsyncContextaSEOS1_": "svc_nn_account_AsyncContext_Assign",

        # nn::friends
        "_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE": "svc_nn_friends_GetFriendList",

        # nn::time
        "_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE": "svc_nn_time_GetCurrentTime",
        "_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE": "svc_nn_time_ToCalendarTime",
        "_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE": "svc_nn_time_GetDeviceLocationName",

        # nn::prepo
        "_ZN2nn5prepo10PlayReportC1Ev": "svc_nn_prepo_PlayReport_C1",
        "_ZN2nn5prepo10PlayReport9SetBufferEPvm": "svc_nn_prepo_PlayReport_SetBuffer",
        "_ZN2nn5prepo10PlayReport5ClearEv": "svc_nn_prepo_PlayReport_Clear",
        "_ZN2nn5prepo10PlayReport10SetEventIdEPKc": "svc_nn_prepo_PlayReport_SetEventId",
        "_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE": "svc_nn_prepo_PlayReport_Save",
        "_ZN2nn5prepo10PlayReport3AddEPKcS3_": "svc_nn_prepo_PlayReport_Add",
        "_ZN2nn5prepo10PlayReport3AddEPKcd": "svc_nn_prepo_PlayReport_Add",
        "_ZN2nn5prepo10PlayReport3AddEPKcl": "svc_nn_prepo_PlayReport_Add",

                # nn::audio
                "_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE": "svc_nn_audio_InitializeAudioOutParameter",
                "_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE": "svc_nn_audio_OpenDefaultAudioOut",
                "_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE": "svc_nn_audio_StartAudioOut",
                "_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE": "svc_nn_audio_StopAudioOut",
                "_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE": "svc_nn_audio_CloseAudioOut",
                "_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE": "svc_nn_audio_AppendAudioOutBuffer",
                "_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE": "svc_nn_audio_GetReleasedAudioOutBuffer",
                "_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE": "svc_nn_audio_GetAudioOutSampleRate",
                "_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE": "svc_nn_audio_GetAudioOutChannelCount",
                "_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE": "svc_nn_audio_GetAudioOutSampleFormat",
                "_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE": "svc_nn_audio_GetAudioOutBufferDataSize",
                "_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE": "svc_nn_audio_GetAudioOutBufferDataPointer",
                "_ZN2nn5audio28GetAudioOutPlayedSampleCountEPKNS0_8AudioOutE": "svc_nn_audio_GetAudioOutPlayedSampleCount",
                "_ZN2nn5audio17GetSampleByteSizeENS0_12SampleFormatE": "svc_nn_audio_GetSampleByteSize",
                "_ZN2nn5audio28InitializePolyphaseResamplerEPNS0_22PolyphaseResamplerTypeEPvmiiibf": "svc_nn_audio_InitializePolyphaseResampler",
                "_ZN2nn5audio31ProcessPolyphaseResamplerBufferEPNS0_22PolyphaseResamplerTypeEPiPsmPKsi": "svc_nn_audio_ProcessPolyphaseResamplerBuffer",
                "_ZN2nn5audio42GetRequiredBufferSizeForPolyphaseResamplerEiiib": "svc_nn_audio_GetRequiredBufferSizeForPolyphaseResampler",
                "_ZN2nn5audio49CalculatePolyphaseResamplerActualOutputSampleRateEii": "svc_nn_audio_CalculatePolyphaseResamplerActualOutputSampleRate",

                # nn::fs
        "_ZN2nn2fs25GetCurrentThreadFsContextEv": "svc_nn_fs_GetCurrentThreadFsContext",
        "_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE": "svc_nn_fs_SetCurrentThreadFsContext",
        "_ZN2nn2fs8MountRomEPKc": "svc_nn_fs_MountRom",
        "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE": "svc_nn_fs_GetFileSize",
        "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci": "svc_nn_fs_OpenFile",
        "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm": "svc_nn_fs_ReadFile",
        "_ZN2nn2fs9CloseFileENS0_10FileHandleE": "svc_nn_fs_CloseFile",

        # nn::err
        "_ZN2nn3err11NplnContext4MakeEPKvm": "svc_nn_err_NplnContext_Make",
        "_ZN2nn3err23UpdateResultWithContextINS0_11NplnContextEEENS_6ResultES3_RKT_": "svc_nn_err_UpdateResultWithContext",
        "_ZN2nn3err18ErrorResultVariantaSERKNS0_9ErrorCodeE": "svc_nn_err_ErrorResultVariant_Assign",
        "_ZN2nn3err18ErrorResultVariantaSERKNS_6ResultE": "svc_nn_err_ErrorResultVariant_Assign",
        "_ZN2nn3err21ReportMultimediaErrorENS_6ResultERKN5movie25MultimediaTelemetryReportE": "svc_nn_err_ReportMultimediaError",

        # nn::ssl
        "_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS1_22BuiltInCertificateInfoEPhjPNS0_15CaCertificateIdEj": "svc_nn_ssl_GetBuiltInCertificates",
        "_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj": "svc_nn_ssl_GetBuiltInCertificateBufSize",

        # nn::socket
        "_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_": "svc_nn_socket_GetAddrInfo",
        "_ZN2nn6socket12FreeAddrInfoEP8addrinfo": "svc_nn_socket_FreeAddrInfo",
        "_ZN2nn6socket8InetNtopEiPKvPcj": "svc_nn_socket_InetNtop",
        "_ZN2nn6socket8InetPtonEiPKcPv": "svc_nn_socket_InetPton",
        "_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE": "svc_nn_socket_GAIStrError",
        "_ZN2nn6socket4BindEiPK8sockaddrj": "svc_nn_socket_Bind",
        "_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj": "svc_nn_socket_SendTo",
        "_ZN2nn6socket6SocketEiii": "svc_nn_socket_Socket",
        "_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj": "svc_nn_socket_RecvFrom",
        "_ZN2nn6socket9InetHtonlEj": "svc_nn_socket_InetHtonl",
        "_ZN2nn6socket9InetNtohlEj": "svc_nn_socket_InetNtohl",
        "_ZN2nn6socket12GetLastErrnoEv": "svc_nn_socket_GetLastErrno",
        "_ZN2nn6socket4PollEP6pollfdmi": "svc_nn_socket_Poll",
        "_ZN2nn6socket8ShutdownEii": "svc_nn_socket_Shutdown",
        "_ZN2nn6socket11EventFdReadEiPm": "svc_nn_socket_EventFdRead",
        "_ZN2nn6socket12EventFdWriteEim": "svc_nn_socket_EventFdWrite",
        "_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE": "svc_nn_socket_EventFd",

        # nn::ldn
        "_ZN2nn3ldn10DisconnectEv": "svc_nn_ldn_Disconnect",
        "_ZN2nn3ldn11OpenStationEv": "svc_nn_ldn_OpenStation",
        "_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi": "svc_nn_ldn_ScanPrivate",
        "_ZN2nn3ldn12CloseStationEv": "svc_nn_ldn_CloseStation",
        "_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE": "svc_nn_ldn_CreateNetwork",
        "_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE": "svc_nn_ldn_ConnectPrivate",
        "_ZN2nn3ldn14DestroyNetworkEv": "svc_nn_ldn_DestroyNetwork",
        "_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE": "svc_nn_ldn_GetIpv4Address",
        "_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE": "svc_nn_ldn_GetNetworkInfo",
        "_ZN2nn3ldn15OpenAccessPointEv": "svc_nn_ldn_OpenAccessPoint",
        "_ZN2nn3ldn16CloseAccessPointEv": "svc_nn_ldn_CloseAccessPoint",
        "_ZN2nn3ldn16SetAdvertiseDataEPKvm": "svc_nn_ldn_SetAdvertiseData",
        "_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE": "svc_nn_ldn_AddAcceptFilterEntry",
        "_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE": "svc_nn_ldn_CreateNetworkPrivate",
        "_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE": "svc_nn_ldn_GetSecurityParameter",
        "_ZN2nn3ldn20MakeBroadcastAddressENS0_11Ipv4AddressENS0_10SubnetMaskE": "svc_nn_ldn_MakeBroadcastAddress",
        "_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE": "svc_nn_ldn_AttachStateChangeEvent",
        "_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE": "svc_nn_ldn_SetStationAcceptPolicy",
        "_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi": "svc_nn_ldn_Scan",
        "_ZN2nn3ldn6RejectENS0_11Ipv4AddressE": "svc_nn_ldn_Reject",
        "_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE": "svc_nn_ldn_Connect",
        "_ZN2nn3ldneqENS0_11Ipv4AddressES1_": "svc_nn_ldn_Ipv4Address_eq",
        "_ZN2nn3ldn19GetDisconnectReasonEv": "svc_nn_ldn_GetDisconnectReason",
        "_ZN2nn3ldn15MakeIpv4AddressEhhhh": "svc_nn_ldn_MakeIpv4Address",

        # nn::ngc
        "_ZN2nn3ngc15ProfanityFilterC1Ev": "svc_nn_ngc_ProfanityFilter_C1",
        "_ZN2nn3ngc15ProfanityFilter10InitializeEPvm": "svc_nn_ngc_ProfanityFilter_Initialize",
        "_ZN2nn3ngc15ProfanityFilter8FinalizeEv": "svc_nn_ngc_ProfanityFilter_Finalize",

        # nn::crypto
        "_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm": "svc_nn_crypto_GenerateCryptographicallyRandomBytes",

        # nn::lmem
        "_ZN2nn4lmem15CreateFrameHeapEPvmi": "svc_nn_lmem_CreateFrameHeap",
        "_ZN2nn4lmem16DestroyFrameHeapEPNS0_6detail8HeapHeadE": "svc_nn_lmem_DestroyFrameHeap",
        "_ZN2nn4lmem21AllocateFromFrameHeapEPNS0_6detail8HeapHeadEmi": "svc_nn_lmem_AllocateFromFrameHeap",
        "_ZN2nn4lmem13CreateExpHeapEPvmi": "svc_nn_lmem_CreateExpHeap",
        "_ZN2nn4lmem13FreeToExpHeapEPNS0_6detail8HeapHeadEPv": "svc_nn_lmem_FreeToExpHeap",
        "_ZN2nn4lmem19AllocateFromExpHeapEPNS0_6detail8HeapHeadEm": "svc_nn_lmem_AllocateFromExpHeap",

        # nn::nsd
        "_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE": "svc_nn_nsd_GetEnvironmentIdentifier",

        # nn::sf::hipc
        "_ZN2nn2sf4cmif16GetInlineContextEv": "svc_nn_sf_cmif_GetInlineContext",
        "_ZN2nn2sf4hipc15SendSyncRequestENS_3svc6HandleEPvm": "svc_nn_sf_hipc_SendSyncRequest",
        "_ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc": "svc_nn_sf_hipc_ConnectToHipcService",
        "_ZN2nn2sf4hipc21GetMessageBufferOnTlsEv": "svc_nn_sf_hipc_GetMessageBufferOnTls",
        "_ZN2nn2sf4hipc24CloseClientSessionHandleENS_3svc6HandleE": "svc_nn_sf_hipc_CloseClientSessionHandle",
        "_ZN2nn2sf4hipc29FinalizeHipcServiceResolutionEv": "svc_nn_sf_hipc_FinalizeHipcServiceResolution",
        "_ZN2nn2sf4hipc31InitializeHipcServiceResolutionEv": "svc_nn_sf_hipc_InitializeHipcServiceResolution",
        "_ZN2nn2sf4hipc6detail23HandleHipcErrorOnClientENS_6ResultE": "svc_nn_sf_hipc_HandleHipcErrorOnClient",
        "_ZN2nn2sf4hipc6detail25HipcMessageBufferAccessor11SetupHeaderEPvmRKNS2_21HipcMessageHeaderInfoE": "svc_nn_sf_hipc_HipcMessageBufferAccessor_SetupHeader",
        "_ZN2nn2sf4hipc6detail25HipcMessageBufferAccessor6SetTagEt": "svc_nn_sf_hipc_HipcMessageBufferAccessor_SetTag",
        "_ZN2nn2sf4impl6detail22ServiceObjectImplBase211ReleaseImplEv": "svc_nn_sf_impl_ServiceObjectImplBase2_ReleaseImpl",
        "_ZN2nn2sf4impl6detail22ServiceObjectImplBase216AddReferenceImplEv": "svc_nn_sf_impl_ServiceObjectImplBase2_AddReferenceImpl",

        # nn::util
        "_ZN2nn4util11ReferSymbolEPKv": "svc_nn_util_ReferSymbol",
        "_ZN2nn4util15RelocationTable8RelocateEv": "svc_nn_util_RelocationTable_Relocate",
        "_ZN2nn4util9VSNPrintfEPcmPKcSt9__va_list": "svc_nn_util_VSNPrintf",
        "_ZN2nn4util6TinyMt10InitializeEPKji": "svc_nn_util_TinyMt_Initialize",
        "_ZN2nn4util6TinyMt17GenerateRandomU32Ev": "svc_nn_util_TinyMt_GenerateRandomU32",
        "_ZN2nn4util8BitArray5resetEv": "svc_nn_util_BitArray_reset",
        "_ZN2nn4util3pmr17NewDeleteResourceEv": "svc_nn_util_pmr_NewDeleteResource",
        "_ZN2nn4util3pmr18GetDefaultResourceEv": "svc_nn_util_pmr_GetDefaultResource",
        "_ZN2nn4util3pmr18SetDefaultResourceEPNS_14MemoryResourceE": "svc_nn_util_pmr_SetDefaultResource",

        # nn::font
        "_ZN2nn6fontll18ScalableFontEngineC1Ev": "svc_nn_font_ScalableFontEngine_C1",
        "_ZN2nn6fontll18ScalableFontEngine7SetFontEPc": "svc_nn_font_ScalableFontEngine_SetFont",
        "_ZN2nn6fontll18ScalableFontEngine8GetErrorEv": "svc_nn_font_ScalableFontEngine_GetError",
        "_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj": "svc_nn_font_ScalableFontEngine_LoadFont",
        "_ZN2nn6fontll18ScalableFontEngine8SetFlagsENS1_5FlagsE": "svc_nn_font_ScalableFontEngine_SetFlags",
        "_ZN2nn6fontll18ScalableFontEngine8SetScaleEiiii": "svc_nn_font_ScalableFontEngine_SetScale",
        "_ZN2nn6fontll24ScalableFontEngineHelper6DecodeEPKvj": "svc_nn_font_ScalableFontEngineHelper_Decode",

        # nn::detail
        "_ZN2nn6detail21UnexpectedDefaultImplEPKcS2_i": "svc_nn_detail_UnexpectedDefaultImpl",
}

PREFIX_SERVICES: list[tuple[str, str]] = [
    ("_ZN2nn", "svc_unimplemented_nn"),
]


@dataclass
class LoadedModule:
    name: str
    nso: Nso
    base: int
    functions: dict[int, Function] = field(default_factory=dict)
    image: bytearray = b""
    relocations_applied: int = 0
    relocations_skipped: list[dict] = field(default_factory=list)
    imports: list[dict] = field(default_factory=list)
    exports: dict[str, int] = field(default_factory=dict)
    lifter: Lifter | None = None   # set when functions were analysed without IR


@dataclass
class Program:
    modules: list[LoadedModule]
    native_bindings: dict[str, str]          # import name -> C symbol
    native_thunks: dict[int, str]            # thunk address -> import name
    thunk_by_name: dict[str, int]            # import name -> thunk address
    trap_imports: list[str]
    report: dict = field(default_factory=dict)
    # OBJECT imports with no definition anywhere get a zeroed 64-byte slot in
    # a synthetic data image so relocations have a real guest address.
    data_slots: dict[str, int] = field(default_factory=dict)
    data_base: int = 0

    @property
    def export_map(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for mod in self.modules:
            for name, addr in mod.exports.items():
                out.setdefault(name, addr)
        return out


def _align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def choose_bases(modules: list[LoadedModule], start: int = 0x100000) -> None:
    base = start
    for mod in modules:
        mod.base = _align(base, 0x10000)
        base = mod.base + _align(mod.nso.image_size, 0x10000)


def _defined_symbols(modules: list[LoadedModule]) -> dict[str, tuple[int, int]]:
    """Collect every defined symbol (FUNC, OBJECT, NOTYPE) in load order.

    ``Nso.exports`` deliberately keeps only code-like symbols, but dynamic
    relocations also target module-local data: RTTI, vtables, static tables and
    string literals.  Those must resolve to their real image address instead of
    being skipped or replaced with a zero-filled slot.
    """
    out: dict[str, tuple[int, int]] = {}
    for mod in modules:
        for sym in mod.nso.symbols():
            if sym.name and not sym.is_import and sym.shndx not in (0xFFF1, 0xFFF2):
                out.setdefault(sym.name, (mod.base, sym.value))
    return out


def _copy_bss_templates(nso, image: bytearray) -> None:
    """Copy bss initialization templates from rodata to bss.

    Some modules have read-only templates in rodata that must be copied to
    zero-initialized bss regions during startup. The rtld normally does this,
    but since we don't run the full rtld, we pre-apply the templates here.
    """
    # Find rodata segment (typically index 1)
    rodata_seg = None
    for seg in nso.segments:
        if seg.index == 1:
            rodata_seg = seg
            break
    if not rodata_seg:
        return

    # Find bss start offset in image (from MOD0)
    mod0 = nso.find_mod0()
    if not mod0:
        return
    bss_start = mod0.bss_start_offset
    bss_end = mod0.bss_end_offset
    if bss_start == 0 or bss_end <= bss_start:
        return

    rodata_start = rodata_seg.mem_offset
    rodata_end = rodata_start + rodata_seg.size

    # Known template regions for sdk module (bss_offset -> size)
    # These offsets are relative to bss_start / rodata_start
    templates = {
        0x74000: 0x300,   # sentinel block at 0x510f000
        0x84000: 0x200,   # function table at 0x511f000
    }

    for offset, size in templates.items():
        src = rodata_start + offset
        dst = bss_start + offset
        if src + size <= rodata_end and dst + size <= len(image):
            # Check if source has non-zero data
            has_data = any(image[src + i] != 0 for i in range(size))
            if has_data:
                image[dst:dst + size] = image[src:src + size]


def _is_native_service_symbol(name: str) -> bool:
    """Check if a symbol should prefer native service over module export."""
    return (
        name.startswith("_ZN2nn2os") or
        name.startswith("_ZN2nn4diag") or
        name.startswith("_ZN2nn6result") or
        name.startswith("_ZN2nn2oe") or
        name.startswith("_ZN2nn8settings") or
        name.startswith("_ZN2nn5swkbd") or
        name.startswith("_ZN2nn3hid") or
        name.startswith("_ZN2nn7account") or
        name.startswith("_ZN2nn7friends") or
        name.startswith("_ZN2nn4time") or
        name.startswith("_ZN2nn5prepo") or
        name.startswith("_ZN2nn2fs") or
        name.startswith("_ZN2nn3err") or
        name.startswith("_ZN2nn3ssl") or
        name.startswith("_ZN2nn6socket") or
        name.startswith("_ZN2nn3ldn") or
        name.startswith("_ZN2nn3ngc") or
        name.startswith("_ZN2nn6crypto") or
        name.startswith("_ZN2nn4lmem") or
        name.startswith("_ZN2nn3nsd") or
        name.startswith("_ZN2nn2sf") or
        name.startswith("_ZN2nn4util") or
        name.startswith("_ZN2nn6font") or
        name.startswith("_ZN2nn6detail")
    )


def build_images(modules: list[LoadedModule], export_map: dict[str, int],
                 thunk_by_name: dict[str, int],
                 data_slots: dict[str, int] | None = None) -> None:
    """Materialise each module image with relocations applied.

    Imports resolve in this order:
      1. a native compatibility service (for Nintendo system services)
      2. an export in any loaded module (static inter-module link)
      3. a symbol defined in this module's own dynamic symbol table
      4. a symbol defined by another loaded module
      5. a generated trap stub, which reports the missing symbol when executed
    """
    defined = _defined_symbols(modules)
    for mod in modules:
        nso = mod.nso
        size = _align(nso.image_size, 0x1000)
        image = bytearray(size)
        for seg in nso.segments:
            if seg.size:
                data = nso.segment_bytes(seg.index)
                image[seg.mem_offset:seg.mem_offset + seg.size] = data

        # Copy bss initialization templates from rodata to bss
        _copy_bss_templates(nso, image)

        mod.image = image

        syms = nso.symbols()
        local_defs: dict[str, int] = {}
        for sym in syms:
            if sym.name and not sym.is_import and sym.shndx not in (0xFFF1, 0xFFF2):
                local_defs.setdefault(sym.name, sym.value)
        for r_offset, r_type, addend, sym_index in nso.parse_dynamic().relocations:
            if r_offset + 8 > len(image):
                mod.relocations_skipped.append({"offset": r_offset, "type": r_type,
                                                "reason": "offset outside image"})
                continue
            if r_type == R_AARCH64_RELATIVE:
                value = (mod.base + addend) & 0xFFFFFFFFFFFFFFFF
            elif r_type in (R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT):
                if sym_index >= len(syms):
                    mod.relocations_skipped.append({"offset": r_offset, "type": r_type,
                                                    "reason": "symbol index %d out of range" % sym_index})
                    continue
                name = syms[sym_index].name
                # Prefer native services for Nintendo system services
                if _is_native_service_symbol(name) and name in thunk_by_name:
                    value = thunk_by_name[name]
                elif name in export_map:
                    value = (export_map[name] + addend) & 0xFFFFFFFFFFFFFFFF
                elif name in local_defs:
                    value = (mod.base + local_defs[name] + addend) & 0xFFFFFFFFFFFFFFFF
                elif name in defined:
                    definer, offset = defined[name]
                    value = (definer + offset + addend) & 0xFFFFFFFFFFFFFFFF
                elif data_slots and name in data_slots:
                    value = (data_slots[name] + addend) & 0xFFFFFFFFFFFFFFFF
                elif name in thunk_by_name:
                    value = thunk_by_name[name]
                else:
                    mod.relocations_skipped.append({"offset": r_offset, "type": r_type,
                                                    "symbol": name, "reason": "unresolved symbol"})
                    continue
            else:
                mod.relocations_skipped.append({"offset": r_offset, "type": r_type,
                                                "reason": "unsupported relocation type"})
                continue
            struct.pack_into("<Q", image, r_offset, value)
            mod.relocations_applied += 1

        for sym in nso.imports:
            mod.imports.append({"name": sym.name, "kind": sym.kind_name})


def resolve_imports(modules: list[LoadedModule], export_map: dict[str, int],
                    data_slots: dict[str, int] | None = None) -> tuple[dict, dict, dict, list]:
    native: dict[str, str] = {}
    traps: list[str] = []
    for mod in modules:
        for sym in mod.nso.imports:
            name = sym.name
            # Don't skip native service symbols even if they're exported by another module
            if not name or (name in export_map and not _is_native_service_symbol(name)) or name in native or name in traps:
                continue
            # OBJECT imports are data (constants, RTTI); a code thunk would be
            # read as a pointer value.  Their slots are allocated separately.
            if sym.kind == 1 and data_slots is not None and name in data_slots:
                continue
            if name in NATIVE_SERVICES:
                native[name] = NATIVE_SERVICES[name]
                continue
            matched = False
            for prefix, cname in PREFIX_SERVICES:
                if name.startswith(prefix):
                    native[name] = cname
                    matched = True
                    break
            if not matched:
                traps.append(name)
    thunk_names = list(native.keys()) + traps
    thunks = {THUNK_BASE + i * 8: name for i, name in enumerate(thunk_names)}
    thunk_by_name = {name: addr for addr, name in thunks.items()}
    return native, thunks, thunk_by_name, sorted(traps)


def plan_data_slots(modules: list[LoadedModule], export_map: dict[str, int]) -> tuple[dict[str, int], int]:
    """Reserve guest memory for OBJECT imports nothing exports."""
    end = 0
    for mod in modules:
        end = max(end, mod.base + _align(mod.nso.image_size, 0x1000))
    base = _align(end, 0x10000)
    names = sorted({sym.name for mod in modules for sym in mod.nso.imports
                    if sym.kind == 1 and sym.name and sym.name not in export_map})
    return {name: base + i * 64 for i, name in enumerate(names)}, base


# Constants the SDK exports as objects rather than code.  They are imported by
# main but not defined in any module of this program, so the slot is seeded
# here instead of leaving zeroes (zeroed pi silently ruins vertex math).
_FLOAT_IMPORTS: list[tuple[str, float]] = [
    ("FloatPiDivided4", 3.14159265358979323846 / 4.0),
    ("FloatPiDivided2", 3.14159265358979323846 / 2.0),
    ("FloatPi", 3.14159265358979323846),
    ("FloatInfinity", float("inf")),
    ("FloatQnan", float("nan")),
    ("FloatNaN", float("nan")),
    ("FloatEpsilon", 1.1920928955078125e-07),
    ("FloatMax", 3.4028234663852886e38),
    ("FloatMin", 1.1754943508222875e-38),
]


def _seed_data_slot(name: str) -> int | None:
    for marker, value in _FLOAT_IMPORTS:
        if marker in name:
            return struct.unpack("<I", struct.pack("<f", value))[0]
    return None


CHUNK_INSTRUCTIONS = 6000   # target ARM instructions per generated .c file


def write_if_changed(path: Path, text: str) -> None:
    """Leave identical files untouched so their mtime (and the build's
    incremental check) only changes where the generated code really changed."""
    try:
        if path.read_text() == text:
            return
    except OSError:
        pass
    path.write_text(text)


# ---- parallel chunk emission -------------------------------------------------
# Emission re-lifts every function (analysis dropped the IR) and turns it into
# C: ~all of the emit time, and independent per function (the Lifter is
# read-only after construction, the Emitter is per function).  Worker processes
# rebuild one Lifter per module from the same inputs and write their chunks;
# the output is byte-identical to the serial path (SW_RECOMP_JOBS=1).
_W_LIFTERS: dict = {}


def _emit_worker_init(lifter_state: dict, fn_index: dict, fn_names: dict | None = None,
                      abi_facts: dict | None = None) -> None:
    global _W_LIFTERS
    _W_LIFTERS = {}
    set_function_index(fn_index, fn_names)
    from ..arm64 import emit as _emit_mod
    _emit_mod.set_abi_facts(abi_facts)
    for key, (text, noreturn, jump_tables, max_bytes, code_start) in lifter_state.items():
        lf = Lifter(text, code_start=code_start, max_function_bytes=max_bytes, noreturn=noreturn)
        lf.jump_tables = jump_tables
        _W_LIFTERS[key] = lf


def _chunk_text(items) -> str:
    """items: (lifter key, fn addr, name, allow_cold_blocks, boundary, base) per function."""
    parts = ['/* generated by swrecomp: %d functions */' % len(items),
             '#include "sw_rt.h"', ""]
    for key, addr, name, cold, boundary, base in items:
        full = _W_LIFTERS[key].lift_function(addr, name=name, allow_cold_blocks=cold, boundary=boundary)
        parts.append(emit_function(full, base))
        parts.append("")
    return "\n".join(parts)


def _emit_worker_chunk(task) -> str:
    path, items = task
    write_if_changed(Path(path), _chunk_text(items))
    return path


def _facts_worker_chunk(task) -> list:
    """ABI facts pass (emit._ABI): lift each function, liveness only."""
    from ..arm64.emit import abi_function_facts
    _path, items = task
    out = []
    for key, addr, name, cold, boundary, base in items:
        full = _W_LIFTERS[key].lift_function(addr, name=name, allow_cold_blocks=cold, boundary=boundary)
        out.append(((addr + base) & 0xFFFFFFFFFFFFFFFF, abi_function_facts(full, base)))
    return out


def _abi_solve(raw: list) -> dict:
    """Whole-program ABI facts from the per-function ones (emit.py _ABI).  Conservative: a tail call passes the
    target's entry needs on to the caller's entry, and a function's return needs to everything it calls or
    tail-calls (kills between are ignored)."""
    entry, ret, tails, callees = {}, {}, {}, {}
    leaf = set()
    ind_post = 0
    for f, (e, calls, tl, cl, lf) in raw:
        if lf:
            leaf.add(f)
        if e:
            entry[f] = set(e)
        tails[f] = [t for t in tl if t is not None]
        callees[f] = cl
        for t, after, _site in calls:
            if t is None:
                ind_post += 1
            else:
                ret.setdefault(t, set()).update(after)
    raw_entry, raw_ret = len(entry), len(ret)
    dump = os.environ.get("SWRECOMP_ABI_DUMP")
    if dump:
        with open(dump, "w") as fh:
            for f, (e, calls, tl, cl, _lf) in raw:
                if e:
                    fh.write("E %x %s\n" % (f, ",".join(sorted(e))))
                for t, after, site in calls:
                    fh.write("C %x %s %x %s\n" % (f, "ind" if t is None else "%x" % t, site, ",".join(sorted(after))))
    changed = True
    while changed:
        changed = False
        for f, tl in tails.items():
            for t in tl:
                e = entry.get(t)
                if e and not e <= entry.get(f, set()):
                    entry.setdefault(f, set()).update(e)
                    changed = True
    changed = True
    while changed:
        changed = False
        for f in list(ret):
            need = ret[f]
            for g in callees.get(f, ()) + tails.get(f, []):
                if not need <= ret.get(g, set()):
                    ret.setdefault(g, set()).update(need)
                    changed = True
    ind = set()
    for e in entry.values():
        ind |= e
    print("[abi] facts: %d functions, entry-scratch %d (%d after tail propagation), return-scratch %d (%d after "
          "propagation), indirect call sites reading scratch after the call %d, indirect union %s"
          % (len(raw), raw_entry, len(entry), raw_ret, len(ret), ind_post, sorted(ind)), flush=True)
    print("[abi] leaf functions (plain C calls, sw_rt.h sw_call_leaf): %d" % len(leaf), flush=True)
    return {"entry": {f: frozenset(v) for f, v in entry.items()},
            "ret": {f: frozenset(v) for f, v in ret.items()}, "ind": frozenset(ind), "leaf": frozenset(leaf)}


def emit_program(program: Program, gen_dir: Path, images_dir: Path,
                 functions_per_file: int = 120) -> dict:
    gen_dir.mkdir(parents=True, exist_ok=True)
    images_dir.mkdir(parents=True, exist_ok=True)

    all_funcs: list[tuple[int, int, Function, Lifter | None]] = []
    used_names: set[str] = set()
    for mod in program.modules:
        for addr, fn in sorted(mod.functions.items()):
            # Lifter names are module-relative, so two modules can produce the
            # same symbol for different code.  Qualify collisions with the
            # module base; calls dispatch by address, so names are labels only.
            if fn.name in used_names or not fn.name:
                fn.name = "sw_f_%08x" % (mod.base + addr)
            while fn.name in used_names:
                fn.name += "_"
            used_names.add(fn.name)
            all_funcs.append((mod.base + addr, mod.base, fn, mod.lifter))
    all_funcs.sort(key=lambda item: item[0])
    # sw_functions[] lists all_funcs in this order (then the native thunks): the emitter passes these
    # indices to sw_call_i / sw_dispatch_i
    fn_index = {addr: i for i, (addr, _b, _f, _l) in enumerate(all_funcs)}
    fn_names = {addr: f.name for addr, _b, f, _l in all_funcs}
    set_function_index(fn_index, fn_names)

    # Function prototypes
    proto = ["/* generated: do not edit */", "#ifndef SW_PROGRAM_H", "#define SW_PROGRAM_H",
             '#include "sw_rt.h"', "",
             "typedef struct SwModuleDesc {",
             "    const char *name;",
             "    u64 base, image_size, text_size, ro_size, data_size, bss_size;",
             "    u64 entry, init_array, init_array_count;",
             "    const char *image_path;",
             "    const char *module_id;",
             "} SwModuleDesc;",
             "typedef struct SwFuncEntry { u64 addr; void (*fn)(SwCpu *); const char *name; } SwFuncEntry;",
             "typedef struct SwNativeEntry { u64 addr; const char *name; } SwNativeEntry;",
             ""]
    for addr, _base, fn, _lifter in all_funcs:
        proto.append("void %s(SwCpu *c);" % fn.name)
    for thunk_addr, name in sorted(program.native_thunks.items()):
        proto.append("void sw_import_%08x(SwCpu *c);" % thunk_addr)
    proto += ["#endif", ""]
    write_if_changed(gen_dir / "sw_program.h", "\n".join(proto))

    # Recompiled code chunks.  Remove chunks from an earlier run: the build
    # compiles every .c in gen/, so a stale chunk (with a different function
    # partition) would define the same symbols twice and fail the link.
    old_chunks = set(gen_dir.glob("sw_chunk_*.c"))
    chunk_files = []
    chunk_count = 0
    # Split by instruction count (capped at functions_per_file functions) so
    # chunks take similar time to compile and the parallel build stays busy.
    chunks: list[list] = [[]]
    weight = 0
    for item in all_funcs:
        n = max(1, item[2].instruction_count)
        if chunks[-1] and (weight + n > CHUNK_INSTRUCTIONS or len(chunks[-1]) >= functions_per_file):
            chunks.append([])
            weight = 0
        chunks[-1].append(item)
        weight += n
    jobs = int(os.environ.get("SW_RECOMP_JOBS", "0") or 0) or (os.cpu_count() or 1)
    lifters = {}
    for _a, _b, fn, lifter in all_funcs:
        if lifter is not None and id(lifter) not in lifters:
            lifters[id(lifter)] = lifter
    parallel = jobs > 1 and all(not fn.blocks and lifter is not None for _a, _b, fn, lifter in all_funcs)
    if parallel:
        from concurrent.futures import ProcessPoolExecutor
        state = {k: (lf.text, set(lf.noreturn), dict(lf.jump_tables), lf.max_function_bytes, lf.code_start)
                 for k, lf in lifters.items()}
        tasks = []
        for chunk in chunks:
            if not chunk:
                continue
            chunk_count += 1
            path = gen_dir / ("sw_chunk_%04d.c" % chunk_count)
            tasks.append((str(path), [(id(lifter), fn.addr, fn.name, fn.allow_cold_blocks, fn.boundary, base)
                                      for _addr, base, fn, lifter in chunk]))
            old_chunks.discard(path)
            chunk_files.append(path)
        from ..arm64 import emit as _emit_mod
        abi_facts = None
        if _emit_mod._ABI:
            import time as _time
            t0 = _time.time()
            raw = []
            with ProcessPoolExecutor(max_workers=jobs, initializer=_emit_worker_init,
                                     initargs=(state, fn_index, fn_names)) as pool:
                for part in pool.map(_facts_worker_chunk, tasks, chunksize=4):
                    raw.extend(part)
            abi_facts = _abi_solve(raw)
            print("[abi] facts pass %.0f s" % (_time.time() - t0), flush=True)
            _emit_mod.set_abi_facts(abi_facts)
            if os.environ.get("SWRECOMP_ABI_FACTS_ONLY") == "1":
                raise SystemExit(0)
        with ProcessPoolExecutor(max_workers=jobs, initializer=_emit_worker_init,
                                 initargs=(state, fn_index, fn_names, abi_facts)) as pool:
            for _ in pool.map(_emit_worker_chunk, tasks, chunksize=4):
                pass
        chunks = []
    for chunk in chunks:
        if not chunk:
            continue
        chunk_count += 1
        path = gen_dir / ("sw_chunk_%04d.c" % chunk_count)
        # Chunks deliberately do NOT include sw_program.h: it declares all ~1M
        # functions (33 MB) and parsing it made each chunk compile 16x slower.
        # Calls between functions go through sw_call/sw_dispatch by address,
        # so a chunk only ever names the functions it defines itself.
        parts = ['/* generated by swrecomp: %d functions */' % len(chunk),
                 '#include "sw_rt.h"', ""]
        for addr, base, fn, lifter in chunk:
            if not fn.blocks and lifter is not None:
                # IR was dropped after analysis: re-lift just this function,
                # emit it, and let it go out of scope again.
                full = lifter.lift_function(fn.addr, name=fn.name,
                                            allow_cold_blocks=fn.allow_cold_blocks,
                                            boundary=fn.boundary)
                parts.append(emit_function(full, base))
            else:
                parts.append(emit_function(fn, base))
            parts.append("")
        write_if_changed(path, "\n".join(parts))
        old_chunks.discard(path)
        chunk_files.append(path)

    # Trap stubs for unresolved imports
    stubs = ['/* generated import stubs */', '#include "sw_rt.h"', ""]
    for thunk_addr, name in sorted(program.native_thunks.items()):
        if name in program.native_bindings:
            cname = NATIVE_SERVICES.get(name)
            if cname is None:
                for prefix, mapped in PREFIX_SERVICES:
                    if name.startswith(prefix):
                        cname = mapped
                        break
            stubs.append("void sw_import_%08x(SwCpu *c) { %s(c); }" % (thunk_addr, cname))
        else:
            stubs.append("void sw_import_%08x(SwCpu *c) { sw_unimplemented_import(c, \"%s\"); }"
                         % (thunk_addr, name.replace('"', "'")))
    # Chunks from an earlier run that this run did not produce would define
    # the same symbols twice; remove them.
    for stale in old_chunks:
        stale.unlink()
    write_if_changed(gen_dir / "sw_imports.c", "\n".join(stubs) + "\n")

    # Program tables + module images
    images = []
    descs = []
    for mod in program.modules:
        image_path = images_dir / ("%s.bin" % mod.name)
        image_path.write_bytes(bytes(mod.image))
        images.append(str(image_path))
        nso = mod.nso
        entry = mod.base + (nso.parse_dynamic().init or 0)
        init_off = nso.parse_dynamic().init_array_offset or 0
        init_count = len(nso.parse_dynamic().init_array)
        descs.append(
            '    {"%s", 0x%xULL, 0x%xULL, 0x%xULL, 0x%xULL, 0x%xULL, 0x%xULL, '
            '0x%xULL, 0x%xULL, %dULL, "%s", "%s"}'
            % (mod.name, mod.base, len(mod.image), nso.segments[0].size,
               nso.segments[1].size, nso.segments[2].size, nso.bss_size,
               entry, mod.base + init_off, init_count,
               "images/" + image_path.name, nso.module_id_hex)
        )

    if program.data_slots:
        blob = bytearray(64 * len(program.data_slots))
        for name, addr in program.data_slots.items():
            seed = _seed_data_slot(name)
            if seed is not None:
                struct.pack_into("<I", blob, addr - program.data_base, seed)
        blob_path = images_dir / "imports.bin"
        blob_path.write_bytes(bytes(blob))
        descs.append(
            '    {"imports", 0x%xULL, 0x%xULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, '
            '"images/%s", "%s"}'
            % (program.data_base, len(blob), blob_path.name, "0" * 64))

    table = ['/* generated program tables */', '#include "sw_rt.h"', '#include "sw_program.h"', "",
             "const SwModuleDesc sw_modules[] = {", ",\n".join(descs), "};",
             "const u32 sw_module_count = %d;" % len(descs), "",
             "const SwFuncEntry sw_functions[] = {"]
    entries = []
    for addr, _base, fn, _lifter in all_funcs:
        entries.append('    {0x%xULL, %s, "%s"}' % (addr, fn.name, fn.name))
    for thunk_addr, name in sorted(program.native_thunks.items()):
        entries.append('    {0x%xULL, sw_import_%08x, "%s"}'
                       % (thunk_addr, thunk_addr, name.replace('"', "'")))
    table.append(",\n".join(entries))
    table += ["};", "const u32 sw_function_count = %d;" % len(entries), "",
              "const SwNativeEntry sw_native_entries[] = {"]
    native_entries = ['    {0x%xULL, "%s"}' % (a, n.replace('"', "'"))
                      for a, n in sorted(program.native_thunks.items())]
    table.append(",\n".join(native_entries) if native_entries else "    {0, 0}")
    table += ["};", "const u32 sw_native_count = %d;" % len(program.native_thunks), ""]
    write_if_changed(gen_dir / "sw_program.c", "\n".join(table))

    manifest = {
        "modules": [
            {
                "name": mod.name,
                "base": mod.base,
                "image_size": len(mod.image),
                "module_id": mod.nso.module_id_hex,
                "function_count": len(mod.functions),
                "relocations_applied": mod.relocations_applied,
                "relocations_skipped": mod.relocations_skipped[:200],
                "imports": len(mod.imports),
                "image_path": str(images_dir / ("%s.bin" % mod.name)),
            }
            for mod in program.modules
        ],
        "functions": len(all_funcs),
        "chunks": [p.name for p in chunk_files],
        "native_bindings": program.native_bindings,
        "trap_imports": program.trap_imports,
    }
    (gen_dir / "program.json").write_text(json.dumps(manifest, indent=2))
    return {"manifest": manifest, "chunks": chunk_files}

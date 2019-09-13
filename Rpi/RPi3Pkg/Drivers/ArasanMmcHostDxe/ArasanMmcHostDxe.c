/** @file
 *
 *  Copyright (c) 2017, Andrei Warkentin <andrey.warkentin@gmail.com>
 *  Copyright (c) Microsoft Corporation. All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include "ArasanMmcHostDxe.h"

#define DEBUG_MMCHOST_SD DEBUG_VERBOSE

BOOLEAN PreviousIsCardPresent = FALSE;
UINT32 LastExecutedCommand = (UINT32) -1;

STATIC RASPBERRY_PI_FIRMWARE_PROTOCOL *mFwProtocol;

/**
   These SD commands are optional, according to the SD Spec
**/
BOOLEAN
IgnoreCommand (
  UINT32 Command
  )
{
  switch (Command) {
  case MMC_CMD20:
    return TRUE;
  default:
    return FALSE;
  }
}

/**
   Translates a generic SD command into the format used by the Arasan SD Host Controller
**/
UINT32
TranslateCommand (
  UINT32 Command,
  UINT32 Argument
  )
{
  UINT32 Translation = 0xffffffff;

  if (LastExecutedCommand == CMD55) {
    switch (Command) {
    case MMC_CMD6:
      Translation = ACMD6;
      DEBUG ((DEBUG_MMCHOST_SD, "ACMD6\n"));
      break;
    case MMC_ACMD22:
      Translation = ACMD22;
      DEBUG ((DEBUG_MMCHOST_SD, "ACMD22\n"));
      break;
    case MMC_ACMD41:
      Translation = ACMD41;
      DEBUG ((DEBUG_MMCHOST_SD, "ACMD41\n"));
      break;
    case MMC_ACMD51:
      Translation = ACMD51;
      DEBUG ((DEBUG_MMCHOST_SD, "ACMD51\n"));
      break;
    default:
      DEBUG ((DEBUG_ERROR, "ArasanMMCHost: TranslateCommand(): Unrecognized App command: %d\n", Command));
    }
  } else {
    switch (Command) {
    case MMC_CMD0:
      Translation = CMD0;
      break;
    case MMC_CMD1:
      Translation = CMD1;
      break;
    case MMC_CMD2:
      Translation = CMD2;
      break;
    case MMC_CMD3:
      Translation = CMD3;
      break;
    case MMC_CMD5:
      Translation = CMD5;
      break;
    case MMC_CMD6:
      Translation = CMD6;
      break;
    case MMC_CMD7:
      Translation = CMD7;
      break;
    case MMC_CMD8: {
      if (Argument == CMD8_SD_ARG) {
        Translation = CMD8_SD;
        DEBUG ((DEBUG_MMCHOST_SD, "Sending SD CMD8 variant\n"));
      } else {
        ASSERT (Argument == CMD8_MMC_ARG);
        Translation = CMD8_MMC;
        DEBUG ((DEBUG_MMCHOST_SD, "Sending MMC CMD8 variant\n"));
      }
      break;
    }
    case MMC_CMD9:
      Translation = CMD9;
      break;
    case MMC_CMD11:
      Translation = CMD11;
      break;
    case MMC_CMD12:
      Translation = CMD12;
      break;
    case MMC_CMD13:
      Translation = CMD13;
      break;
    case MMC_CMD16:
      Translation = CMD16;
      break;
    case MMC_CMD17:
      Translation = CMD17;
      break;
    case MMC_CMD18:
      Translation = CMD18;
      break;
    case MMC_CMD23:
      Translation = CMD23;
      break;
    case MMC_CMD24:
      Translation = CMD24;
      break;
    case MMC_CMD25:
      Translation = CMD25;
      break;
    case MMC_CMD55:
      Translation = CMD55;
      break;
    default:
      DEBUG ((DEBUG_ERROR, "ArasanMMCHost: TranslateCommand(): Unrecognized Command: %d\n", Command));
    }
  }

  return Translation;
}

/**
   Repeatedly polls a register until its value becomes correct, or until MAX_RETRY_COUNT polls is reached
**/
EFI_STATUS
PollRegisterWithMask (
  IN UINTN Register,
  IN UINTN Mask,
  IN UINTN ExpectedValue
  )
{
  UINTN RetryCount = 0;

  while (RetryCount < MAX_RETRY_COUNT) {
    if ((MmioRead32 (Register) & Mask) != ExpectedValue) {
      RetryCount++;
      gBS->Stall (STALL_AFTER_RETRY_US);
    } else {
      break;
    }
  }

  if (RetryCount == MAX_RETRY_COUNT) {
    return EFI_TIMEOUT;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SoftReset (
  IN UINT32 Mask
  )
{
  MmioOr32 (MMCHS_SYSCTL, Mask);
  if (PollRegisterWithMask (MMCHS_SYSCTL, Mask, 0) == EFI_TIMEOUT) {
    DEBUG ((DEBUG_ERROR, "Failed to SoftReset with mask 0x%x\n", Mask));
    return EFI_TIMEOUT;
  }

  return EFI_SUCCESS;
}

/**
   Calculate the clock divisor
**/
EFI_STATUS
CalculateClockFrequencyDivisor (
  IN UINTN TargetFrequency,
  OUT UINT32 *DivisorValue,
  OUT UINTN *ActualFrequency
  )
{
  EFI_STATUS Status;
  UINT32 Divisor;
  UINT32 BaseFrequency = 0;

  Status = mFwProtocol->GetClockRate (RPI_MBOX_CLOCK_RATE_EMMC, &BaseFrequency);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Couldn't get RPI_MBOX_CLOCK_RATE_EMMC\n"));
    return Status;
  }

  ASSERT (BaseFrequency != 0);
  Divisor = BaseFrequency / TargetFrequency;

  // Arasan controller is based on 3.0 spec so the div is multiple of 2
  // Actual Frequency = BaseFequency/(Div*2)
  Divisor /= 2;

  if ((TargetFrequency < BaseFrequency) &&
      (TargetFrequency * 2 * Divisor != BaseFrequency)) {
    Divisor += 1;
  }

  if (Divisor > MAX_DIVISOR_VALUE) {
    Divisor = MAX_DIVISOR_VALUE;
  }

  DEBUG ((DEBUG_MMCHOST_SD, "ArasanMMCHost: BaseFrequency 0x%x Divisor 0x%x\n", BaseFrequency, Divisor));

  *DivisorValue = (Divisor & 0xFF) << 8;
  Divisor >>= 8;
  *DivisorValue |= (Divisor & 0x03) << 6;

  if (ActualFrequency) {
    if (Divisor == 0) {
      *ActualFrequency = BaseFrequency;
    } else {
      *ActualFrequency = BaseFrequency / Divisor;
      *ActualFrequency >>= 1;
    }
    DEBUG ((DEBUG_MMCHOST_SD, "ArasanMMCHost: *ActualFrequency 0x%x\n", *ActualFrequency));
  }

  DEBUG ((DEBUG_MMCHOST_SD, "ArasanMMCHost: *DivisorValue 0x%x\n", *DivisorValue));

  return EFI_SUCCESS;
}

BOOLEAN
MMCIsCardPresent (
  IN EFI_MMC_HOST_PROTOCOL *This
)
{
  return TRUE;
}

BOOLEAN
MMCIsReadOnly (
  IN EFI_MMC_HOST_PROTOCOL *This
  )
{
  BOOLEAN IsReadOnly = !((MmioRead32 (MMCHS_PRES_STATE) & WRITE_PROTECT_OFF) == WRITE_PROTECT_OFF);
  DEBUG ((DEBUG_MMCHOST_SD, "ArasanMMCHost: MMCIsReadOnly(): %d\n", IsReadOnly));
  return IsReadOnly;
}

EFI_STATUS
MMCBuildDevicePath (
  IN EFI_MMC_HOST_PROTOCOL       *This,
  IN EFI_DEVICE_PATH_PROTOCOL    **DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL *NewDevicePathNode;
  EFI_GUID DevicePathGuid = EFI_CALLER_ID_GUID;

  DEBUG ((DEBUG_MMCHOST_SD, "ArasanMMCHost: MMCBuildDevicePath()\n"));

  NewDevicePathNode = CreateDeviceNode (HARDWARE_DEVICE_PATH, HW_VENDOR_DP, sizeof (VENDOR_DEVICE_PATH));
  CopyGuid (&((VENDOR_DEVICE_PATH*) NewDevicePathNode)->Guid, &DevicePathGuid);
  *DevicePath = NewDevicePathNode;

  return EFI_SUCCESS;
}

EFI_STATUS
MMCSendCommand (
  IN EFI_MMC_HOST_PROTOCOL    *This,
  IN MMC_CMD                  MmcCmd,
  IN UINT32                   Argument
  )
{
  UINTN MmcStatus;
  UINTN RetryCount = 0;
  UINTN CmdSendOKMask;
  EFI_STATUS Status = EFI_SUCCESS;
  BOOLEAN IsAppCmd = (LastExecutedCommand == CMD55);
  BOOLEAN IsDATCmd = FALSE;
  BOOLEAN IsADTCCmd = FALSE;

  DEBUG ((DEBUG_MMCHOST_SD, "ArasanMMCHost: MMCSendCommand(MmcCmd: %08x, Argument: %08x)\n", MmcCmd, Argument));

  if (IgnoreCommand (MmcCmd)) {
    return EFI_SUCCESS;
  }

  MmcCmd = TranslateCommand (MmcCmd, Argument);
  if (MmcCmd == 0xffffffff) {
    return EFI_UNSUPPORTED;
  }

  if ((MmcCmd & CMD_R1_ADTC) == CMD_R1_ADTC) {
    IsADTCCmd = TRUE;
  }
  if (((MmcCmd & CMD_R1B) == CMD_R1B &&
    /*
     * Abort commands don't get inhibited by DAT.
     */
    (MmcCmd & TYPE (CMD_TYPE_ABORT)) != TYPE (CMD_TYPE_ABORT)) ||
    IsADTCCmd ||
    /*
     * We want to detect BRR/BWR change.
     */
    MmcCmd == CMD_SEND_STATUS) {
    IsDATCmd = TRUE;
  }

  CmdSendOKMask = CMDI_MASK;
  if (IsDATCmd) {
    CmdSendOKMask |= DATI_MASK;
  }

  if (PollRegisterWithMask (MMCHS_PRES_STATE,
    CmdSendOKMask, 0) == EFI_TIMEOUT) {
    DEBUG ((DEBUG_ERROR, "%a(%u): not ready for MMC_CMD%u PresState 0x%x MmcStatus 0x%x\n",
      __FUNCTION__, __LINE__, MMC_CMD_NUM (MmcCmd),
      MmioRead32 (MMCHS_PRES_STATE), MmioRead32 (MMCHS_INT_STAT)));
    Status = EFI_TIMEOUT;
    goto Exit;
  }

  if (IsAppCmd && MmcCmd == ACMD22) {
    MmioWrite32 (MMCHS_BLK, 4);
  } else if (IsAppCmd && MmcCmd == ACMD51) {
    MmioWrite32 (MMCHS_BLK, 8);
  } else if (!IsAppCmd && MmcCmd == CMD6) {
    MmioWrite32 (MMCHS_BLK, 64);
  } else if (IsADTCCmd) {
    MmioWrite32 (MMCHS_BLK, BLEN_512BYTES);
  }

  // Set Data timeout counter value to max value.
  MmioAndThenOr32 (MMCHS_SYSCTL, (UINT32) ~DTO_MASK, DTO_VAL);

  //
  // Clear Interrupt Status Register, but not the Card Inserted bit
  // to avoid messing with card detection logic.
  //
  MmioWrite32 (MMCHS_INT_STAT, ALL_EN & ~(CARD_INS));

  // Set command argument register
  MmioWrite32 (MMCHS_ARG, Argument);

  // Send the command
  MmioWrite32 (MMCHS_CMD, MmcCmd);

  // Check for the command status.
  while (RetryCount < MAX_RETRY_COUNT) {
    MmcStatus = MmioRead32 (MMCHS_INT_STAT);

    // Read status of command response
    if ((MmcStatus & ERRI) != 0) {
      //
      // CMD5 (CMD_IO_SEND_OP_COND) is only valid for SDIO
      // cards and thus expected to fail.
      //
      if (MmcCmd != CMD_IO_SEND_OP_COND) {
        DEBUG ((DEBUG_ERROR, "%a(%u): MMC_CMD%u ERRI MmcStatus 0x%x\n",
          __FUNCTION__, __LINE__, MMC_CMD_NUM (MmcCmd), MmcStatus));
      }

      SoftReset (SRC);

      Status = EFI_DEVICE_ERROR;
      goto Exit;
    }

    // Check if command is completed.
    if ((MmcStatus & CC) == CC) {
      MmioWrite32 (MMCHS_INT_STAT, CC);
      break;
    }

    RetryCount++;
    gBS->Stall (STALL_AFTER_RETRY_US);
  }

  gBS->Stall (STALL_AFTER_SEND_CMD_US);

  if (RetryCount == MAX_RETRY_COUNT) {
    DEBUG ((DEBUG_ERROR, "%a(%u): MMC_CMD%u completion TIMEOUT PresState 0x%x MmcStatus 0x%x\n",
      __FUNCTION__, __LINE__, MMC_CMD_NUM (MmcCmd),
      MmioRead32 (MMCHS_PRES_STATE), MmcStatus));
    Status = EFI_TIMEOUT;
    goto Exit;
  }

Exit:
  if (EFI_ERROR (Status)) {
    LastExecutedCommand = (UINT32) -1;
  } else {
    LastExecutedCommand = MmcCmd;
  }
  return Status;
}

EFI_STATUS
MMCNotifyState (
  IN EFI_MMC_HOST_PROTOCOL    *This,
  IN MMC_STATE                State
  )
{
  EFI_STATUS Status;
  UINTN ClockFrequency;
  UINT32 Divisor;

  DEBUG ((DEBUG_MMCHOST_SD, "ArasanMMCHost: MMCNotifyState(State: %d)\n", State));

  switch (State) {
  case MmcHwInitializationState:
    {
      EFI_STATUS Status;
      UINT32 Divisor;

      Status = SoftReset (SRA);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      // Attempt to set the clock to 400Khz which is the expected initialization speed
      Status = CalculateClockFrequencyDivisor (400000, &Divisor, NULL);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "ArasanMMCHost: MMCNotifyState(): Fail to initialize SD clock\n"));
        return Status;
      }

      // Set Data Timeout Counter value, set clock frequency, enable internal clock
      MmioOr32 (MMCHS_SYSCTL, DTO_VAL | Divisor | CEN | ICS | ICE);

      // Enable interrupts
      MmioWrite32 (MMCHS_IE, ALL_EN);
    }
    break;
  case MmcIdleState:
    break;
  case MmcReadyState:
    break;
  case MmcIdentificationState:
    break;
  case MmcStandByState:
    ClockFrequency = 25000000;

    // First turn off the clock
    MmioAnd32 (MMCHS_SYSCTL, ~CEN);

    Status = CalculateClockFrequencyDivisor (ClockFrequency, &Divisor, NULL);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "ArasanMMCHost: MmcStandByState(): Fail to initialize SD clock to %u Hz\n",
        ClockFrequency));
      return Status;
    }

    // Setup new divisor
    MmioAndThenOr32 (MMCHS_SYSCTL, (UINT32) ~CLKD_MASK, Divisor);

    // Wait for the clock to stabilise
    while ((MmioRead32 (MMCHS_SYSCTL) & ICS_MASK) != ICS);

    // Set Data Timeout Counter value, set clock frequency, enable internal clock
    MmioOr32 (MMCHS_SYSCTL, CEN);
    break;
  case MmcTransferState:
    break;
  case MmcSendingDataState:
    break;
  case MmcReceiveDataState:
    break;
  case MmcProgrammingState:
    break;
  case MmcDisconnectState:
  case MmcInvalidState:
  default:
    DEBUG ((DEBUG_ERROR, "ArasanMMCHost: MMCNotifyState(): Invalid State: %d\n", State));
    ASSERT (0);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
MMCReceiveResponse (
  IN EFI_MMC_HOST_PROTOCOL    *This,
  IN MMC_RESPONSE_TYPE        Type,
  IN UINT32*                  Buffer
  )
{
  ASSERT (Buffer != NULL);

  if (Type == MMC_RESPONSE_TYPE_R2) {

    // 16-byte response
    Buffer[0] = MmioRead32 (MMCHS_RSP10);
    Buffer[1] = MmioRead32 (MMCHS_RSP32);
    Buffer[2] = MmioRead32 (MMCHS_RSP54);
    Buffer[3] = MmioRead32 (MMCHS_RSP76);

    Buffer[3] <<= 8;
    Buffer[3] |= (Buffer[2] >> 24) & 0xFF;
    Buffer[2] <<= 8;
    Buffer[2] |= (Buffer[1] >> 24) & 0xFF;
    Buffer[1] <<= 8;
    Buffer[1] |= (Buffer[0] >> 24) & 0xFF;
    Buffer[0] <<= 8;

    DEBUG ((DEBUG_MMCHOST_SD,
      "ArasanMMCHost: MMCReceiveResponse(Type: %x), Buffer[0-3]: %08x, %08x, %08x, %08x\n",
      Type, Buffer[0], Buffer[1], Buffer[2], Buffer[3]));
  } else {
    // 4-byte response
    Buffer[0] = MmioRead32 (MMCHS_RSP10);
    DEBUG ((DEBUG_MMCHOST_SD, "ArasanMMCHost: MMCReceiveResponse(Type: %08x), Buffer[0]: %08x\n", Type, Buffer[0]));
  }

  gBS->Stall (STALL_AFTER_REC_RESP_US);
  if (LastExecutedCommand == CMD_STOP_TRANSMISSION) {
    DEBUG ((DEBUG_MMCHOST_SD, "ArasanMMCHost: soft-resetting after CMD12\n"));
    return SoftReset (SRC | SRD);
  }
  return EFI_SUCCESS;
}

EFI_STATUS
MMCReadBlockData (
  IN EFI_MMC_HOST_PROTOCOL    *This,
  IN EFI_LBA                  Lba,
  IN UINTN                    Length,
  IN UINT32*                  Buffer
  )
{
  UINTN MmcStatus;
  UINTN RemLength;
  UINTN Count;

  DEBUG ((DEBUG_VERBOSE, "%a(%u): LBA: 0x%x, Length: 0x%x, Buffer: 0x%x)\n",
    __FUNCTION__, __LINE__, Lba, Length, Buffer));

  if (Buffer == NULL) {
    DEBUG ((DEBUG_ERROR, "%a(%u): NULL Buffer\n", __FUNCTION__, __LINE__));
    return EFI_INVALID_PARAMETER;
  }

  if (Length % sizeof (UINT32) != 0) {
    DEBUG ((DEBUG_ERROR, "%a(%u): bad Length %u\n", __FUNCTION__, __LINE__, Length));
    return EFI_INVALID_PARAMETER;
  }

  RemLength = Length;
  while (RemLength != 0) {
    UINTN RetryCount = 0;
    UINT32 BlockLen = MIN (RemLength, BLEN_512BYTES);

    while (RetryCount < MAX_RETRY_COUNT) {
      MmcStatus = MmioRead32 (MMCHS_INT_STAT);
      if ((MmcStatus & BRR) != 0) {
        MmioWrite32 (MMCHS_INT_STAT, BRR);
        /*
         * Data is ready.
         */
        mFwProtocol->SetLed (TRUE);
        for (Count = 0; Count < BlockLen; Count += 4, Buffer++) {
          *Buffer = MmioRead32 (MMCHS_DATA);
        }

        mFwProtocol->SetLed (FALSE);
        break;
      }

      gBS->Stall (STALL_AFTER_RETRY_US);
      RetryCount++;
    }

    if (RetryCount == MAX_RETRY_COUNT) {
      DEBUG ((DEBUG_ERROR, "%a(%u): %lu/%lu MMCHS_INT_STAT: %08x\n",
        __FUNCTION__, __LINE__, Length - RemLength, Length, MmcStatus));
      return EFI_TIMEOUT;
    }

    RemLength -= BlockLen;
    gBS->Stall (STALL_AFTER_READ_US);
  }

  MmioWrite32 (MMCHS_INT_STAT, BRR);
  return EFI_SUCCESS;
}

EFI_STATUS
MMCWriteBlockData (
  IN EFI_MMC_HOST_PROTOCOL    *This,
  IN EFI_LBA                  Lba,
  IN UINTN                    Length,
  IN UINT32*                  Buffer
  )
{
  UINTN MmcStatus;
  UINTN RemLength;
  UINTN Count;

  DEBUG ((DEBUG_VERBOSE, "%a(%u): LBA: 0x%x, Length: 0x%x, Buffer: 0x%x)\n",
    __FUNCTION__, __LINE__, Lba, Length, Buffer));

  if (Buffer == NULL) {
    DEBUG ((DEBUG_ERROR, "%a(%u): NULL Buffer\n", __FUNCTION__, __LINE__));
    return EFI_INVALID_PARAMETER;
  }

  if (Length % sizeof (UINT32) != 0) {
    DEBUG ((DEBUG_ERROR, "%a(%u): bad Length %u\n", __FUNCTION__, __LINE__, Length));
    return EFI_INVALID_PARAMETER;
  }

  RemLength = Length;
  while (RemLength != 0) {
    UINTN RetryCount = 0;
    UINT32 BlockLen = MIN (RemLength, BLEN_512BYTES);

    while (RetryCount < MAX_RETRY_COUNT) {
      MmcStatus = MmioRead32 (MMCHS_INT_STAT);
      if ((MmcStatus & BWR) != 0) {
        MmioWrite32 (MMCHS_INT_STAT, BWR);
        /*
         * Can write data.
         */
        mFwProtocol->SetLed (TRUE);
        for (Count = 0; Count < BlockLen; Count += 4, Buffer++) {
          MmioWrite32 (MMCHS_DATA, *Buffer);
        }

        mFwProtocol->SetLed (FALSE);
        break;
      }

      gBS->Stall (STALL_AFTER_RETRY_US);
      RetryCount++;
    }

    if (RetryCount == MAX_RETRY_COUNT) {
      DEBUG ((DEBUG_ERROR, "%a(%u): %lu/%lu MMCHS_INT_STAT: %08x\n",
        __FUNCTION__, __LINE__, Length - RemLength, Length, MmcStatus));
      return EFI_TIMEOUT;
    }

    RemLength -= BlockLen;
    gBS->Stall (STALL_AFTER_WRITE_US);
  }

  MmioWrite32 (MMCHS_INT_STAT, BWR);
  return EFI_SUCCESS;
}

BOOLEAN
MMCIsMultiBlock (
  IN EFI_MMC_HOST_PROTOCOL *This
  )
{
  return TRUE;
}

EFI_MMC_HOST_PROTOCOL gMMCHost =
{
  MMC_HOST_PROTOCOL_REVISION,
  MMCIsCardPresent,
  MMCIsReadOnly,
  MMCBuildDevicePath,
  MMCNotifyState,
  MMCSendCommand,
  MMCReceiveResponse,
  MMCReadBlockData,
  MMCWriteBlockData,
  NULL,
  MMCIsMultiBlock
};

EFI_STATUS
MMCInitialize (
  IN EFI_HANDLE          ImageHandle,
  IN EFI_SYSTEM_TABLE    *SystemTable
  )
{
  EFI_STATUS Status;
  EFI_HANDLE Handle = NULL;

  DEBUG ((DEBUG_MMCHOST_SD, "ArasanMMCHost: MMCInitialize()\n"));

  if (!PcdGet32 (PcdSdIsArasan)) {
    DEBUG ((DEBUG_INFO, "SD is not routed to Arasan\n"));
    return EFI_REQUEST_UNLOAD_IMAGE;
  }

  Status = gBS->LocateProtocol (&gRaspberryPiFirmwareProtocolGuid, NULL,
                  (VOID**)&mFwProtocol);
  ASSERT_EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gRaspberryPiMmcHostProtocolGuid,
                  &gMMCHost,
                  NULL
                );
  ASSERT_EFI_ERROR (Status);

  return Status;
}

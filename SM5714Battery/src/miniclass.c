/*++

Copyright (c) Microsoft Corporation. All rights reserved.

Module Name:

	miniclass.c

Abstract:

	This module implements battery miniclass functionality specific to the
	SM5714 PMIC battery driver.

	N.B. This code is provided "AS IS" without any expressed or implied warranty.

--*/

//--------------------------------------------------------------------- Includes

#include "..\inc\SM5714Battery.h"
#include "..\inc\Spb.h"
#include "usbfnbase.h"
#include "miniclass.tmh"

#include "..\inc\SM5714Battery_regs.h"

//------------------------------------------------------------------- Prototypes

_IRQL_requires_same_
VOID
SM5714BatteryUpdateTag(
	_Inout_ PSM5714_BATTERY_FDO_DATA DevExt
);

BCLASS_QUERY_TAG_CALLBACK SM5714BatteryQueryTag;
BCLASS_QUERY_INFORMATION_CALLBACK SM5714BatteryQueryInformation;
BCLASS_SET_INFORMATION_CALLBACK SM5714BatterySetInformation;
BCLASS_QUERY_STATUS_CALLBACK SM5714BatteryQueryStatus;
BCLASS_SET_STATUS_NOTIFY_CALLBACK SM5714BatterySetStatusNotify;
BCLASS_DISABLE_STATUS_NOTIFY_CALLBACK SM5714BatteryDisableStatusNotify;

//---------------------------------------------------------------------- Pragmas

#pragma alloc_text(PAGE, SM5714BatteryPrepareHardware)
#pragma alloc_text(PAGE, SM5714BatteryUpdateTag)
#pragma alloc_text(PAGE, SM5714BatteryQueryTag)
#pragma alloc_text(PAGE, SM5714BatteryQueryInformation)
#pragma alloc_text(PAGE, SM5714BatteryQueryStatus)
#pragma alloc_text(PAGE, SM5714BatterySetStatusNotify)
#pragma alloc_text(PAGE, SM5714BatteryDisableStatusNotify)
#pragma alloc_text(PAGE, SM5714BatterySetInformation)
//------------------------------------------------------------ Battery Interface

_Use_decl_annotations_
VOID
SM5714BatteryPrepareHardware(
	WDFDEVICE Device
)

/*++

Routine Description:

	This routine is called to initialize battery data to sane values.

Arguments:

	Device - Supplies the device to initialize.

Return Value:

	NTSTATUS

--*/

{

	PSM5714_BATTERY_FDO_DATA DevExt;
	NTSTATUS Status = STATUS_SUCCESS;

	PAGED_CODE();
	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Entering %!FUNC!\n");

	DevExt = GetDeviceExtension(Device);


	WdfWaitLockAcquire(DevExt->StateLock, NULL);
	SM5714BatteryUpdateTag(DevExt);
	WdfWaitLockRelease(DevExt->StateLock);

	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Leaving %!FUNC!: Status = 0x%08lX\n", Status);
	return;
}

_Use_decl_annotations_
VOID
SM5714BatteryUpdateTag(PSM5714_BATTERY_FDO_DATA DevExt)

/*++

Routine Description:

	This routine is called when static battery properties have changed to
	update the battery tag.

Arguments:

	DevExt - Supplies a pointer to the device extension  of the battery to
		update.

Return Value:

	None

--*/

{

	PAGED_CODE();

	DevExt->BatteryTag += 1;
	if (DevExt->BatteryTag == BATTERY_TAG_INVALID) {
		DevExt->BatteryTag += 1;
	}

	return;
}

_Use_decl_annotations_
NTSTATUS
SM5714BatteryQueryTag(
	PVOID Context,
	PULONG BatteryTag
)

/*++

Routine Description:

	This routine is called to get the value of the current battery tag.

Arguments:

	Context - Supplies the miniport context value for battery

	BatteryTag - Supplies a pointer to a ULONG to receive the battery tag.

Return Value:

	NTSTATUS

--*/

{
	PSM5714_BATTERY_FDO_DATA DevExt;
	NTSTATUS Status;

	PAGED_CODE();
	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Entering %!FUNC!\n");

	DevExt = (PSM5714_BATTERY_FDO_DATA)Context;
	WdfWaitLockAcquire(DevExt->StateLock, NULL);
	*BatteryTag = DevExt->BatteryTag;
	WdfWaitLockRelease(DevExt->StateLock);
	if (*BatteryTag == BATTERY_TAG_INVALID) {
		Status = STATUS_NO_SUCH_DEVICE;
	}
	else {
		Status = STATUS_SUCCESS;
	}

	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Leaving %!FUNC!: Status = 0x%08lX\n", Status);
	return Status;
}

NTSTATUS
SM5714BatteryQueryBatteryInformation(PSM5714_BATTERY_FDO_DATA DevExt, PBATTERY_INFORMATION BatteryInformationResult)
{
	NTSTATUS Status;

	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Entering %!FUNC!\n");

	// Set up battery info (chemistry, capacity in mWh, etc.)
	BatteryInformationResult->Capabilities = BATTERY_SYSTEM_BATTERY;
	BatteryInformationResult->Technology = 1; // Li-Ion

	BYTE LION[4] = {'L','I','O','N'};
	RtlCopyMemory(BatteryInformationResult->Chemistry, LION, 4);

	// For a 4500 mAh Li-ion battery at 4.4 V:
	BatteryInformationResult->DesignedCapacity = 19800; // mWh (4500mAh * 4.4V)
	BatteryInformationResult->FullChargedCapacity = 19228; // mWh (4370mAh * 4.4V)

	BatteryInformationResult->DefaultAlert1 = BatteryInformationResult->FullChargedCapacity * 7 / 100; // 7% of total capacity for error
	BatteryInformationResult->DefaultAlert2 = BatteryInformationResult->FullChargedCapacity * 9 / 100; // 9% of total capacity for warning
	BatteryInformationResult->CriticalBias = 0;

	//
	// Fetch cycle count over I2C
	//
	int  CycleCount = 0;
	unsigned short rawCycle = 0;

	Status = SpbWriteRead(&DevExt->I2CContext, (PVOID)write_cycle, sizeof(write_cycle), &readCmd, sizeof(readCmd), &rawCycle, sizeof(rawCycle), 0);
	if (!NT_SUCCESS(Status))
	{
		Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "Failed to SPB write/read raw cycle count. Status=0x%08lX\n", Status);
		goto Exit;
	}

	if (rawCycle < 0) {
		CycleCount = 0;
	}
	else {
		CycleCount = rawCycle & 0x00FF;
	}

	BatteryInformationResult->CycleCount = CycleCount;

	Trace(
		TRACE_LEVEL_INFORMATION,
		SM5714_BATTERY_TRACE,
		"BATTERY_INFORMATION: \n"
		"Capabilities: %d \n"
		"Technology: %d \n"
		"DesignedCapacity: %d \n"
		"FullChargedCapacity: %d \n"
		"DefaultAlert1: %d \n"
		"DefaultAlert2: %d \n"
		"CriticalBias: %d \n"
		"CycleCount: %d\n",
		BatteryInformationResult->Capabilities,
		BatteryInformationResult->Technology,
		BatteryInformationResult->DesignedCapacity,
		BatteryInformationResult->FullChargedCapacity,
		BatteryInformationResult->DefaultAlert1,
		BatteryInformationResult->DefaultAlert2,
		BatteryInformationResult->CriticalBias,
		BatteryInformationResult->CycleCount);

Exit:
	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Leaving %!FUNC!: Status = 0x%08lX\n", Status);
	return Status;
}

/*
NTSTATUS
SM5714BatteryQueryBatteryEstimatedTime(
	PSM5714_BATTERY_FDO_DATA DevExt,
	LONG AtRate,
	PULONG ResultValue
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	UCHAR Flags = 0;
	UINT16 ETA = 0;

	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Entering %!FUNC!\n");

	if (AtRate == 0)
	{
		Status = SpbReadDataSynchronously(&DevExt->I2CContext, 0x0A, &Flags, 2);
		if (!NT_SUCCESS(Status))
		{
			Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "SpbReadDataSynchronously failed with Status = 0x%08lX\n", Status);
			goto Exit;
		}

		if (Flags & (1 << 0) || Flags & (1 << 1))
		{
			Status = SpbReadDataSynchronously(&DevExt->I2CContext, 0x16, &ETA, 2);
			if (!NT_SUCCESS(Status))
			{
				Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "SpbReadDataSynchronously failed with Status = 0x%08lX\n", Status);
				goto Exit;
			}

			if (ETA == 0xFFFF)
			{
				*ResultValue = BATTERY_UNKNOWN_TIME;

				Trace(
					TRACE_LEVEL_INFORMATION,
					SM5714_BATTERY_TRACE,
					"BatteryEstimatedTime: BATTERY_UNKNOWN_TIME\n");
			}
			else
			{
				*ResultValue = ETA * 60; // Seconds

				Trace(
					TRACE_LEVEL_INFORMATION,
					SM5714_BATTERY_TRACE,
					"BatteryEstimatedTime: %d seconds\n",
					*ResultValue);
			}
		}
		else
		{
			*ResultValue = BATTERY_UNKNOWN_TIME;

			Trace(
				TRACE_LEVEL_INFORMATION,
				SM5714_BATTERY_TRACE,
				"BatteryEstimatedTime: BATTERY_UNKNOWN_TIME\n");
		}
	}
	else
	{
		*ResultValue = BATTERY_UNKNOWN_TIME;

		Trace(
			TRACE_LEVEL_INFORMATION,
			SM5714_BATTERY_TRACE,
			"BatteryEstimatedTime: BATTERY_UNKNOWN_TIME for AtRate = %d\n",
			AtRate);
	}

Exit:
	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE,
		"Leaving %!FUNC!: Status = 0x%08lX\n",
		Status);
	return Status;
}
*/

_Use_decl_annotations_
NTSTATUS
SM5714BatteryQueryInformation(
	PVOID Context,
	ULONG BatteryTag,
	BATTERY_QUERY_INFORMATION_LEVEL Level,
	LONG AtRate,
	PVOID Buffer,
	ULONG BufferLength,
	PULONG ReturnedLength
)

/*++

Routine Description:

	Called by the class driver to retrieve battery information

	The battery class driver will serialize all requests it issues to
	the miniport for a given battery.

	Return invalid parameter when a request for a specific level of information
	can't be handled. This is defined in the battery class spec.

Arguments:

	Context - Supplies the miniport context value for battery

	BatteryTag - Supplies the tag of current battery

	Level - Supplies the type of information required

	AtRate - Supplies the rate of drain for the BatteryEstimatedTime level

	Buffer - Supplies a pointer to a buffer to place the information

	BufferLength - Supplies the length in bytes of the buffer

	ReturnedLength - Supplies the length in bytes of the returned data

Return Value:

	Success if there is a battery currently installed, else no such device.

--*/

{
	PSM5714_BATTERY_FDO_DATA DevExt;
	ULONG ResultValue;
	PVOID ReturnBuffer;
	size_t ReturnBufferLength;
	NTSTATUS Status;

	BATTERY_REPORTING_SCALE ReportingScale = { 0 };
	BATTERY_INFORMATION BatteryInformationResult = { 0 };
	WCHAR StringResult[MAX_BATTERY_STRING_SIZE] = { 0 };
	BATTERY_MANUFACTURE_DATE ManufactureDate = { 0 };

	unsigned short rawTemp = 0;
	int Temperature = 0;
	USHORT DateData = 0;

	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Entering %!FUNC!\n");
	PAGED_CODE();

	DevExt = (PSM5714_BATTERY_FDO_DATA)Context;
	WdfWaitLockAcquire(DevExt->StateLock, NULL);
	if (BatteryTag != DevExt->BatteryTag) {
		Status = STATUS_NO_SUCH_DEVICE;
		goto QueryInformationEnd;
	}

	//
	// Determine the value of the information being queried for and return it.
	//

	ReturnBuffer = NULL;
	ReturnBufferLength = 0;
	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_INFO, "Query for information level 0x%x\n", Level);
	Status = STATUS_INVALID_DEVICE_REQUEST;
	switch (Level) {
	case BatteryInformation:
		Status = SM5714BatteryQueryBatteryInformation(DevExt, &BatteryInformationResult);
		if (!NT_SUCCESS(Status))
		{
			Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "SM5714BatteryQueryBatteryInformation failed with Status = 0x%08lX\n", Status);
			goto Exit;
		}

		ReturnBuffer = &BatteryInformationResult;
		ReturnBufferLength = sizeof(BATTERY_INFORMATION);
		Status = STATUS_SUCCESS;
		break;

	/*
	case BatteryEstimatedTime:
		Status = SM5714BatteryQueryBatteryEstimatedTime(DevExt, AtRate, &ResultValue);
		if (!NT_SUCCESS(Status))
		{
			Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "SM5714BatteryQueryBatteryEstimatedTime failed with Status = 0x%08lX\n", Status);
			goto Exit;
		}

		ReturnBuffer = &ResultValue;
		ReturnBufferLength = sizeof(ResultValue);
		Status = STATUS_SUCCESS;
		break;
	*/

	case BatteryUniqueID:

		swprintf_s(StringResult, sizeof(StringResult) / sizeof(WCHAR), L"%c%c%c%c%c%c%c%c",
			'S',
			'M',
			'5',
			'7',
			'1',
			'4',
			'F',
			'G');

		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "BatteryUniqueID: %S\n", StringResult);

		Status = RtlStringCbLengthW(StringResult, sizeof(StringResult), &ReturnBufferLength);
		if (!NT_SUCCESS(Status))
		{
			Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "RtlStringCbLengthW failed with Status = 0x%08lX\n", Status);
			goto Exit;
		}

		ReturnBuffer = StringResult;
		ReturnBufferLength += sizeof(WCHAR);
		Status = STATUS_SUCCESS;
		break;


	case BatteryManufactureName:

		swprintf_s(StringResult, sizeof(StringResult) / sizeof(WCHAR), L"%c%c",
			0x53,  // S
			0x53   // S
		);

		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "BatteryManufactureName: %S\n", StringResult);

		Status = RtlStringCbLengthW(StringResult, sizeof(StringResult), &ReturnBufferLength);
		if (!NT_SUCCESS(Status))
		{
			Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "RtlStringCbLengthW failed with Status = 0x%08lX\n", Status);
			goto Exit;
		}

		ReturnBuffer = StringResult;
		ReturnBufferLength += sizeof(WCHAR);
		Status = STATUS_SUCCESS;
		break;

	case BatteryDeviceName:

		swprintf_s(StringResult, sizeof(StringResult) / sizeof(WCHAR), L"%c%c%c%c%c%c",
			0x53,  // S
			0x4D,  // M
			0x35,  // 5
			0x37,  // 7
			0x31,  // 1
			0x34   // 4
		);

		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "BatteryDeviceName: %S\n", StringResult);

		Status = RtlStringCbLengthW(StringResult, sizeof(StringResult), &ReturnBufferLength);
		if (!NT_SUCCESS(Status))
		{
			Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "RtlStringCbLengthW failed with Status = 0x%08lX\n", Status);
			goto Exit;
		}

		ReturnBuffer = StringResult;
		ReturnBufferLength += sizeof(WCHAR);
		Status = STATUS_SUCCESS;
		break;

	case BatterySerialNumber:

		swprintf_s(StringResult, sizeof(StringResult) / sizeof(WCHAR), L"%u", (UINT32)5714);

		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "BatterySerialNumber: %S\n", StringResult);

		Status = RtlStringCbLengthW(StringResult, sizeof(StringResult), &ReturnBufferLength);
		if (!NT_SUCCESS(Status))
		{
			Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "RtlStringCbLengthW failed with Status = 0x%08lX\n", Status);
			goto Exit;
		}

		ReturnBuffer = StringResult;
		ReturnBufferLength += sizeof(WCHAR);
		Status = STATUS_SUCCESS;
		break;

	case BatteryManufactureDate:

		ManufactureDate.Day = 1;
		ManufactureDate.Month = 9;
		ManufactureDate.Year = 2021;

		ReturnBuffer = &ManufactureDate;
		ReturnBufferLength = sizeof(BATTERY_MANUFACTURE_DATE);
		Status = STATUS_SUCCESS;
		break;

	case BatteryGranularityInformation:

		ReportingScale.Capacity = 4500;
		ReportingScale.Granularity = 1;

		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "BATTERY_REPORTING_SCALE: Capacity: %d, Granularity: %d\n", ReportingScale.Capacity, ReportingScale.Granularity);

		ReturnBuffer = &ReportingScale;
		ReturnBufferLength = sizeof(BATTERY_REPORTING_SCALE);
		Status = STATUS_SUCCESS;
		break;

	//
	// Fetch temperature over I2C
	//
	case BatteryTemperature:

		Status = SpbWriteRead(&DevExt->I2CContext, (PVOID)write_temperature, sizeof(write_temperature), &readCmd, sizeof(readCmd), &rawTemp, sizeof(rawTemp), 0);
		if (!NT_SUCCESS(Status))
		{
			Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "Failed to SPB write/read raw battery temperature. Status=0x%08lX\n", Status);
		}

		if (rawTemp < 0) {
			Temperature = 0;
		}
		else {
			Temperature = ((rawTemp & 0x7fff) >> 8) * 10;                  //integer bit
			Temperature = Temperature + (((rawTemp & 0x00f0) * 10) / 256); // integer + fractional bit
			if (rawTemp & 0x8000)
				Temperature *= -1;
		}

		Temperature = (ULONG)Temperature / (ULONG)10;

		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Battery temperature: %d\n", Temperature);

		ReturnBuffer = &Temperature;
		ReturnBufferLength = sizeof(ULONG);
		Status = STATUS_SUCCESS;
		break;

	default:
		Status = STATUS_INVALID_PARAMETER;
		break;
	}

Exit:
	NT_ASSERT(((ReturnBufferLength == 0) && (ReturnBuffer == NULL)) ||
		((ReturnBufferLength > 0) && (ReturnBuffer != NULL)));

	if (NT_SUCCESS(Status)) {
		*ReturnedLength = (ULONG)ReturnBufferLength;
		if (ReturnBuffer != NULL) {
			if ((Buffer == NULL) || (BufferLength < ReturnBufferLength)) {
				Status = STATUS_BUFFER_TOO_SMALL;

			}
			else {
				memcpy(Buffer, ReturnBuffer, ReturnBufferLength);
			}
		}

	}
	else {
		*ReturnedLength = 0;
	}

QueryInformationEnd:
	WdfWaitLockRelease(DevExt->StateLock);
	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Leaving %!FUNC!: Status = 0x%08lX\n", Status);
	return Status;
}

_Use_decl_annotations_
NTSTATUS
SM5714BatteryQueryStatus(
	PVOID Context,
	ULONG BatteryTag,
	PBATTERY_STATUS BatteryStatus
)

/*++

Routine Description:

	Called by the class driver to retrieve the batteries current status

	The battery class driver will serialize all requests it issues to
	the miniport for a given battery.

Arguments:

	Context - Supplies the miniport context value for battery

	BatteryTag - Supplies the tag of current battery

	BatteryStatus - Supplies a pointer to the structure to return the current
		battery status in

Return Value:

	Success if there is a battery currently installed, else no such device.

--*/

{
	PSM5714_BATTERY_FDO_DATA DevExt;
	NTSTATUS Status;
	INT16 Rate = 0;
	UCHAR Flags = 0;

	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Entering %!FUNC!\n");
	PAGED_CODE();

	DevExt = (PSM5714_BATTERY_FDO_DATA)Context;
	WdfWaitLockAcquire(DevExt->StateLock, NULL);
	if (BatteryTag != DevExt->BatteryTag) {
		Status = STATUS_NO_SUCH_DEVICE;
		goto QueryStatusEnd;
	}

	//
	// Fetch State of Charge over I2C
	//
	unsigned int     Capacity = 0;
	unsigned short rawCapacity = 0;

	Status = SpbWriteRead(&DevExt->I2CContext, (PVOID)write_capacity, sizeof(write_capacity), &readCmd, sizeof(readCmd), &rawCapacity, sizeof(rawCapacity), 0);
	if (!NT_SUCCESS(Status))
	{
		Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "Failed to SPB write/read raw State of Charge. Status=0x%08lX\n", Status);
	}
	
	Capacity = FIXED_POINT_8_8_EXTEND_TO_INT((unsigned short)rawCapacity, 10);

	//
	// Fetch Voltage(mV) over I2C
	//
	unsigned int  Voltage = 0;
	unsigned short rawOcv = 0;

	Status = SpbWriteRead(&DevExt->I2CContext, (PVOID)write_ocv, sizeof(write_ocv), &readCmd, sizeof(readCmd), &rawOcv, sizeof(rawOcv), 0);
	if (!NT_SUCCESS(Status))
	{
		Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "Failed to SPB write/read raw voltage. Status=0x%08lX\n", Status);
	}

	if (rawOcv < 0) {
		Voltage = 4000;
	}
	else {
		Voltage = ((rawOcv & 0x3800) >> 11) * 1000;         //integer;
		Voltage = Voltage + (((rawOcv & 0x07ff) * 1000) / 2048); // integer + fractional
	}

	//
	// Fetch Current (mA) over I2C
	//
	int            Current = 0;
	unsigned short rawCurr = 0;

	Status = SpbWriteRead(&DevExt->I2CContext, (PVOID)write_current, sizeof(write_current), &readCmd, sizeof(readCmd), &rawCurr, sizeof(rawCurr), 0);
	if (!NT_SUCCESS(Status))
	{
		Trace(TRACE_LEVEL_ERROR, SM5714_BATTERY_TRACE, "Failed to SPB write/read raw current. Status=0x%08lX\n", Status);
	}
	Current = ((rawCurr & 0x1800) >> 11) * 1000; //integer;
	Current = Current + (((rawCurr & 0x07ff) * 1000) / 2048); // integer + fractional
	if (rawCurr & 0x8000)
		Current *= -1;

	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "CURRENT: %d mA", Current);


	//
	// Fetch battery power state (use a dirty workaround for now)
	//
	if (Current >= 30) {
		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "BATTERY_POWER_ON_LINE\n");
		BatteryStatus->PowerState = BATTERY_POWER_ON_LINE;
	}
	else {
		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "BATTERY_DISCHARGING\n");
		BatteryStatus->PowerState = BATTERY_DISCHARGING;
	}

	/*
	 * BatteryStatus expects:
	 * - Capacity in mWh
	 * - Voltage in mV
	 * - Rate in mW (signed)
	 */

	// (4370mAh * 4.4V = 19228 mWh)
	BatteryStatus->Capacity = (ULONG)Capacity * 19228 / (ULONG)1000;
	// mV
	BatteryStatus->Voltage = (ULONG)Voltage;
	// mW (Signed)
	BatteryStatus->Rate = (((LONG)Current * (LONG)Voltage) / (LONG)1000);

	// Debug: Print final BatteryStatus
	Trace(
		TRACE_LEVEL_INFORMATION,
		SM5714_BATTERY_TRACE,
		"BATTERY_STATUS: \n"
		"PowerState: %d \n"
		"Capacity: %d \n"
		"Voltage: %d \n"
		"Rate: %d\n",
		BatteryStatus->PowerState,
		BatteryStatus->Capacity,
		BatteryStatus->Voltage,
		BatteryStatus->Rate);

	Status = STATUS_SUCCESS;

QueryStatusEnd:
	WdfWaitLockRelease(DevExt->StateLock);
	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Leaving %!FUNC!: Status = 0x%08lX\n", Status);
	return Status;
}

_Use_decl_annotations_
NTSTATUS
SM5714BatterySetStatusNotify(
	PVOID Context,
	ULONG BatteryTag,
	PBATTERY_NOTIFY BatteryNotify
)

/*++

Routine Description:

	Called by the class driver to set the capacity and power state levels
	at which the class driver requires notification.

	The battery class driver will serialize all requests it issues to
	the miniport for a given battery.

Arguments:

	Context - Supplies the miniport context value for battery

	BatteryTag - Supplies the tag of current battery

	BatteryNotify - Supplies a pointer to a structure containing the
		notification critera.

Return Value:

	Success if there is a battery currently installed, else no such device.

--*/

{
	PSM5714_BATTERY_FDO_DATA DevExt;
	NTSTATUS Status;

	UNREFERENCED_PARAMETER(BatteryNotify);

	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Entering %!FUNC!\n");
	PAGED_CODE();

	DevExt = (PSM5714_BATTERY_FDO_DATA)Context;
	WdfWaitLockAcquire(DevExt->StateLock, NULL);
	if (BatteryTag != DevExt->BatteryTag) {
		Status = STATUS_NO_SUCH_DEVICE;
		goto SetStatusNotifyEnd;
	}

	Status = STATUS_NOT_SUPPORTED;

SetStatusNotifyEnd:
	WdfWaitLockRelease(DevExt->StateLock);
	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Leaving %!FUNC!: Status = 0x%08lX\n", Status);
	return Status;
}

_Use_decl_annotations_
NTSTATUS
SM5714BatteryDisableStatusNotify(
	PVOID Context
)

/*++

Routine Description:

	Called by the class driver to disable notification.

	The battery class driver will serialize all requests it issues to
	the miniport for a given battery.

Arguments:

	Context - Supplies the miniport context value for battery

Return Value:

	Success if there is a battery currently installed, else no such device.

--*/

{
	NTSTATUS Status;

	UNREFERENCED_PARAMETER(Context);

	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Entering %!FUNC!\n");
	PAGED_CODE();

	Status = STATUS_NOT_SUPPORTED;
	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Leaving %!FUNC!: Status = 0x%08lX\n", Status);
	return Status;
}

_Use_decl_annotations_
NTSTATUS
SM5714BatterySetInformation(
	PVOID Context,
	ULONG BatteryTag,
	BATTERY_SET_INFORMATION_LEVEL Level,
	PVOID Buffer
)

/*
 Routine Description:

	Called by the class driver to set the battery's charge/discharge state,
	critical bias, or charge current.

Arguments:

	Context - Supplies the miniport context value for battery

	BatteryTag - Supplies the tag of current battery

	Level - Supplies action requested

	Buffer - Supplies a critical bias value if level is BatteryCriticalBias.

Return Value:

	NTSTATUS

--*/

{
	PBATTERY_CHARGING_SOURCE ChargingSource;
	PULONG CriticalBias;
	PBATTERY_CHARGER_ID ChargerId;
	PBATTERY_CHARGER_STATUS ChargerStatus;
	PBATTERY_USB_CHARGER_STATUS UsbChargerStatus;
	USBFN_PORT_TYPE UsbFnPortType;
	PSM5714_BATTERY_FDO_DATA DevExt;
	NTSTATUS Status;

	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Entering %!FUNC!\n");
	PAGED_CODE();

	DevExt = (PSM5714_BATTERY_FDO_DATA)Context;
	WdfWaitLockAcquire(DevExt->StateLock, NULL);
	if (BatteryTag != DevExt->BatteryTag) {
		Status = STATUS_NO_SUCH_DEVICE;
		goto SetInformationEnd;
	}

	if (Level == BatteryCharge)
	{
		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_INFO, "SM5714Battery : BatteryCharge\n");

		Status = STATUS_SUCCESS;
	}
	else if (Level == BatteryDischarge)
	{
		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_INFO, "SM5714Battery : BatteryDischarge\n");

		Status = STATUS_SUCCESS;
	}
	else if (Buffer == NULL)
	{
		Status = STATUS_INVALID_PARAMETER_4;
	}
	else if (Level == BatteryChargingSource)
	{
		ChargingSource = (PBATTERY_CHARGING_SOURCE)Buffer;

		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_INFO, "SM5714Battery : BatteryChargingSource Type = %d\n", ChargingSource->Type);

		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_INFO, "SM5714Battery : Set MaxCurrentDraw = %u mA\n", ChargingSource->MaxCurrent);

		Status = STATUS_SUCCESS;
	}
	else if (Level == BatteryCriticalBias)
	{
		CriticalBias = (PULONG)Buffer;
		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_INFO, "SM5714Battery : Set CriticalBias = %u mW\n", *CriticalBias);

		Status = STATUS_SUCCESS;
	}
	else if (Level == BatteryChargerId)
	{
		ChargerId = (PBATTERY_CHARGER_ID)Buffer;
		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_INFO, "SM5714Battery : BatteryChargerId = %!GUID!\n", ChargerId);

		Status = STATUS_SUCCESS;
	}
	else if (Level == BatteryChargerStatus)
	{
		ChargerStatus = (PBATTERY_CHARGER_STATUS)Buffer;

		Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_INFO, "SM5714Battery : BatteryChargingSource Type = %d\n", ChargerStatus->Type);

		if (ChargerStatus->Type == BatteryChargingSourceType_USB)
		{
			UsbChargerStatus = (PBATTERY_USB_CHARGER_STATUS)Buffer;

			Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_INFO,
				"SM5714Battery : BatteryChargingSourceType_USB: Flags = %d, MaxCurrent = %d, Voltage = %d, PortType = %d, PortId = %llu, OemCharger = %!GUID!\n",
				UsbChargerStatus->Flags, UsbChargerStatus->MaxCurrent, UsbChargerStatus->Voltage, UsbChargerStatus->PortType, UsbChargerStatus->PortId, &UsbChargerStatus->OemCharger);

			UsbFnPortType = (USBFN_PORT_TYPE)(UINT64)UsbChargerStatus->PowerSourceInformation;

			Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_INFO, "SM5714Battery : UsbFnPortType = %d\n", UsbFnPortType);
		}

		Status = STATUS_SUCCESS;
	}
	else
	{
		Status = STATUS_NOT_SUPPORTED;
	}

SetInformationEnd:
	WdfWaitLockRelease(DevExt->StateLock);
	Trace(TRACE_LEVEL_INFORMATION, SM5714_BATTERY_TRACE, "Leaving %!FUNC!: Status = 0x%08lX\n", Status);
	return Status;
}
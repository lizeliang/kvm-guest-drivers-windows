/**********************************************************************
 * Copyright (c) 2010  Red Hat, Inc.
 *
 * File: VIOSerialDevice.c
 *
 * Placeholder for the device related functions
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
**********************************************************************/

#include "precomp.h"
#include "vioser.h"

#if defined(EVENT_TRACING)
#include "Device.tmh"
#endif

EVT_WDF_DEVICE_PREPARE_HARDWARE     VIOSerialEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     VIOSerialEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY             VIOSerialEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT              VIOSerialEvtDeviceD0Exit;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED VIOSerialEvtDeviceD0EntryPostInterruptsEnabled;


static NTSTATUS VIOSerialInitInterruptHandling(IN WDFDEVICE hDevice);
static NTSTATUS VIOSerialInitAllQueues(IN WDFOBJECT hDevice);
static NTSTATUS VIOSerialShutDownAllQueues(IN WDFOBJECT WdfDevice);

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, VIOSerialEvtDeviceAdd)
#pragma alloc_text (PAGE, VIOSerialEvtDevicePrepareHardware)
#pragma alloc_text (PAGE, VIOSerialEvtDeviceReleaseHardware)
#pragma alloc_text (PAGE, VIOSerialEvtDeviceD0Exit)
#pragma alloc_text (PAGE, VIOSerialEvtDeviceD0EntryPostInterruptsEnabled)

#endif

static
NTSTATUS
VIOSerialInitInterruptHandling(
    IN WDFDEVICE hDevice)
{
    WDF_OBJECT_ATTRIBUTES        attributes;
    WDF_INTERRUPT_CONFIG         interruptConfig;
    PPORTS_DEVICE	         pContext = GetPortsDevice(hDevice);
    NTSTATUS                     status = STATUS_SUCCESS;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_HW_ACCESS, "<--> %s\n", __FUNCTION__);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PORTS_DEVICE);
    WDF_INTERRUPT_CONFIG_INIT(
                                 &interruptConfig,
                                 VIOSerialInterruptIsr,
                                 VIOSerialInterruptDpc
                                 );

    interruptConfig.EvtInterruptEnable = VIOSerialInterruptEnable;
    interruptConfig.EvtInterruptDisable = VIOSerialInterruptDisable;

    status = WdfInterruptCreate(
                                 hDevice,
                                 &interruptConfig,
                                 &attributes,
                                 &pContext->WdfInterrupt
                                 );

    if (!NT_SUCCESS (status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_HW_ACCESS, "WdfInterruptCreate failed: %x\n", status);
        return status;
    }

    return status;
}

NTSTATUS 
VIOSerialEvtDeviceAdd(
    IN WDFDRIVER Driver,
    IN PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS                     status = STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES        Attributes;
    WDFDEVICE                    hDevice;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_CHILD_LIST_CONFIG        ChildListConfig;
    PNP_BUS_INFORMATION          busInfo;
	
    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "<--> %s\n", __FUNCTION__);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDevicePrepareHardware = VIOSerialEvtDevicePrepareHardware;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = VIOSerialEvtDeviceReleaseHardware;
    PnpPowerCallbacks.EvtDeviceD0Entry         = VIOSerialEvtDeviceD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit          = VIOSerialEvtDeviceD0Exit;
    PnpPowerCallbacks.EvtDeviceD0EntryPostInterruptsEnabled = VIOSerialEvtDeviceD0EntryPostInterruptsEnabled;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    WDF_CHILD_LIST_CONFIG_INIT(
                                 &ChildListConfig,
                                 sizeof(VIOSERIAL_PORT),
                                 VIOSerialDeviceListCreatePdo
                                 );

    ChildListConfig.EvtChildListIdentificationDescriptionDuplicate =
                                 VIOSerialEvtChildListIdentificationDescriptionDuplicate;

    ChildListConfig.EvtChildListIdentificationDescriptionCompare =
                                 VIOSerialEvtChildListIdentificationDescriptionCompare;

    ChildListConfig.EvtChildListIdentificationDescriptionCleanup =
                                 VIOSerialEvtChildListIdentificationDescriptionCleanup;

    WdfFdoInitSetDefaultChildListConfig(
                                 DeviceInit,
                                 &ChildListConfig,
                                 WDF_NO_OBJECT_ATTRIBUTES
                                 );

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, PORTS_DEVICE);
    Attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&DeviceInit, &Attributes, &hDevice);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfDeviceCreate failed - 0x%x\n", status);
        return status;
    }

    status = VIOSerialInitInterruptHandling(hDevice);
    if(!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "VIOSerialInitInterruptHandling failed - 0x%x\n", status);
    }

    status = WdfDeviceCreateDeviceInterface(
                                 hDevice,
                                 &GUID_VIOSERIAL_CONTROLLER,
                                 NULL
                                 );
    if (!NT_SUCCESS(status)) 
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfDeviceCreateDeviceInterface failed - 0x%x\n", status);
        return status;
    }

    busInfo.BusTypeGuid = GUID_DEVCLASS_PORT_DEVICE;
    busInfo.LegacyBusType = PNPBus;
    busInfo.BusNumber = 0;

    WdfDeviceSetBusInformationForChildren(hDevice, &busInfo);

    return status;
}

NTSTATUS 
VIOSerialEvtDevicePrepareHardware(
    IN WDFDEVICE Device,
    IN WDFCMRESLIST ResourcesRaw,
    IN WDFCMRESLIST ResourcesTranslated)
{
    int nListSize = 0;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR pResDescriptor;
    int i = 0;
    PPORTS_DEVICE pContext = GetPortsDevice(Device);
    bool bPortFound = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    UINT nr_ports;
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "<--> %s\n", __FUNCTION__);

    nListSize = WdfCmResourceListGetCount(ResourcesTranslated);

    for (i = 0; i < nListSize; i++)
    {
        if(pResDescriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, i))
        {
            switch(pResDescriptor->Type)
            {
                case CmResourceTypePort :
                    pContext->bPortMapped = (pResDescriptor->Flags & CM_RESOURCE_PORT_IO) ? FALSE : TRUE;
                    pContext->PortBasePA = pResDescriptor->u.Port.Start;
                    pContext->uPortLength = pResDescriptor->u.Port.Length;
                    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "IO Port Info  [%08I64X-%08I64X]\n",
                                 pResDescriptor->u.Port.Start.QuadPart,
                                 pResDescriptor->u.Port.Start.QuadPart +
                                 pResDescriptor->u.Port.Length);

                    if (pContext->bPortMapped )
                    {
                        pContext->pPortBase = MmMapIoSpace(pContext->PortBasePA,
                                                           pContext->uPortLength,
                                                           MmNonCached);

                        if (!pContext->pPortBase) {
                            TraceEvents(TRACE_LEVEL_ERROR, DBG_HW_ACCESS, "%s>>> Failed to map IO port!\n", __FUNCTION__);
                            return STATUS_INSUFFICIENT_RESOURCES;
                        }
                    }
                    else
                    {
                        pContext->pPortBase = (PVOID)(ULONG_PTR)pContext->PortBasePA.QuadPart;
                    }

                    bPortFound = TRUE;

                    break;
                case CmResourceTypeInterrupt:
                    break;
            }
        }
    }

    if(!bPortFound)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_HW_ACCESS, "%s>>> %s", __FUNCTION__, "IO port wasn't found!\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    VirtIODeviceSetIOAddress(&pContext->IODevice, (ULONG_PTR)pContext->pPortBase);

    VirtIODeviceReset(&pContext->IODevice);

    VirtIODeviceAddStatus(&pContext->IODevice, VIRTIO_CONFIG_S_ACKNOWLEDGE);

    pContext->consoleConfig.max_nr_ports = 1;
    if(pContext->isHostMultiport = VirtIODeviceGetHostFeature(&pContext->IODevice, VIRTIO_CONSOLE_F_MULTIPORT))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "We have multiport host\n");
        VirtIODeviceEnableGuestFeature(&pContext->IODevice, VIRTIO_CONSOLE_F_MULTIPORT);
        VirtIODeviceGet(&pContext->IODevice,
                                 FIELD_OFFSET(CONSOLE_CONFIG, max_nr_ports),
                                 &pContext->consoleConfig.max_nr_ports,
                                 sizeof(pContext->consoleConfig.max_nr_ports));
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                                "VirtIOConsoleConfig->max_nr_ports %d\n", pContext->consoleConfig.max_nr_ports);
    }

    if(pContext->isHostMultiport)
    {
        WDF_OBJECT_ATTRIBUTES  attributes;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = Device;
        status = WdfSpinLockCreate(
                                &attributes,
                                &pContext->CVqLock
                                );
        if (!NT_SUCCESS(status))
        {
           TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                "WdfSpinLockCreate failed 0x%x\n", status);
           return status;
        }
    }
    else
    {
//FIXME
//        VIOSerialAddPort(Device, 0);
    }

    nr_ports = pContext->consoleConfig.max_nr_ports;
    pContext->in_vqs = ExAllocatePoolWithTag(
                                 NonPagedPool,
                                 nr_ports * sizeof(struct virtqueue*),
                                 VIOSERIAL_DRIVER_MEMORY_TAG);

    if(pContext->in_vqs == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,"ExAllocatePoolWithTag failed\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
    }

    pContext->out_vqs = ExAllocatePoolWithTag(
                                 NonPagedPool,
                                 nr_ports * sizeof(struct virtqueue*),
                                 VIOSERIAL_DRIVER_MEMORY_TAG
                                 );

    if(pContext->out_vqs == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "ExAllocatePoolWithTag failed\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
    }

    pContext->DeviceOK = TRUE;
    return status;
}

NTSTATUS 
VIOSerialEvtDeviceReleaseHardware(
    IN WDFDEVICE Device,
    IN WDFCMRESLIST ResourcesTranslated)
{
    PPORTS_DEVICE pContext = GetPortsDevice(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
	
    PAGED_CODE();
	
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "<--> %s\n", __FUNCTION__);
	
    if (pContext->pPortBase && pContext->bPortMapped) 
    {
        MmUnmapIoSpace(pContext->pPortBase, pContext->uPortLength);
    }

    pContext->pPortBase = (ULONG_PTR)NULL;

    if(pContext->in_vqs)
    {
        ExFreePoolWithTag(pContext->in_vqs, VIOSERIAL_DRIVER_MEMORY_TAG);
        pContext->in_vqs = NULL;
    }

    if(pContext->out_vqs)
    {
        ExFreePoolWithTag(pContext->out_vqs, VIOSERIAL_DRIVER_MEMORY_TAG);
        pContext->out_vqs = NULL;
    }

    return STATUS_SUCCESS;
}

static 
NTSTATUS 
VIOSerialInitAllQueues(
    IN WDFOBJECT Device)
{
    NTSTATUS               status = STATUS_SUCCESS;
    PPORTS_DEVICE          pContext = GetPortsDevice(Device);
    UINT                   nr_ports, i, j;
    struct virtqueue       *in_vq, *out_vq;
    WDF_OBJECT_ATTRIBUTES  attributes;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<--> %s\n", __FUNCTION__);
    
    nr_ports = pContext->consoleConfig.max_nr_ports;

    for(i = 0, j = 0; i < nr_ports; i++)
    {
        in_vq  = VirtIODeviceFindVirtualQueue(&pContext->IODevice, i * 2, NULL);
        out_vq = VirtIODeviceFindVirtualQueue(&pContext->IODevice, (i * 2 ) + 1, NULL);

        if(i == 1) // Control Port
        {
           pContext->c_ivq = in_vq;
           pContext->c_ovq = out_vq;
        }
        else
        {
           pContext->in_vqs[j] = in_vq;
           pContext->out_vqs[j] = out_vq;
           ++j;
        }
    }

    if(pContext->isHostMultiport)
    {
        ASSERT(pContext->c_ivq);
        status = VIOSerialFillQueue(pContext->c_ivq, pContext->CVqLock);
    }

    return status;
}

NTSTATUS 
VIOSerialShutDownAllQueues(
    IN WDFOBJECT WdfDevice)
{
    NTSTATUS		status = STATUS_SUCCESS;
    PPORTS_DEVICE	pContext = GetPortsDevice(WdfDevice);
    UINT                nr_ports, i;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    VirtIODeviceRemoveStatus(&pContext->IODevice , VIRTIO_CONFIG_S_DRIVER_OK);

    if(pContext->isHostMultiport)
    {
        if(pContext->c_ivq)
        {
            pContext->c_ivq->vq_ops->shutdown(pContext->c_ivq);
            VirtIODeviceDeleteVirtualQueue(pContext->c_ivq);
            pContext->c_ivq = NULL;
        }
        if(pContext->c_ovq)
        {
            pContext->c_ovq->vq_ops->shutdown(pContext->c_ovq);
            VirtIODeviceDeleteVirtualQueue(pContext->c_ovq);
            pContext->c_ovq = NULL;
        }
    } 

    nr_ports = pContext->consoleConfig.max_nr_ports - 1;
    for(i = 0; i < nr_ports; i++ )
    {
        if(pContext->in_vqs && pContext->in_vqs[i])
        {
            pContext->in_vqs[i]->vq_ops->shutdown(pContext->in_vqs[i]);
            VirtIODeviceDeleteVirtualQueue(pContext->in_vqs[i]);
            pContext->in_vqs[i] = NULL;
        }

        if(pContext->out_vqs && pContext->out_vqs[i])
        {
            pContext->out_vqs[i]->vq_ops->shutdown(pContext->out_vqs[i]);
            VirtIODeviceDeleteVirtualQueue(pContext->out_vqs[i]);
            pContext->out_vqs[i] = NULL;
        }
    }
    return status;
}

NTSTATUS 
VIOSerialFillQueue(
    IN struct virtqueue *vq,
    IN WDFSPINLOCK Lock
)
{
    NTSTATUS     status = STATUS_SUCCESS;
    PPORT_BUFFER buf = NULL;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "<--> %s\n", __FUNCTION__);

    for (;;)
    {
        buf = VIOSerialAllocateBuffer(PAGE_SIZE);
        if(buf == NULL)
        {
           TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "VIOSerialAllocateBuffer failed\n");
           return STATUS_INSUFFICIENT_RESOURCES;
        }

        WdfSpinLockAcquire(Lock);
        status = VIOSerialAddInBuf(vq, buf);
        if(!NT_SUCCESS(status))
        {
           VIOSerialFreeBuffer(buf);
           WdfSpinLockRelease(Lock);
           break;
        }
        WdfSpinLockRelease(Lock);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
VIOSerialEvtDeviceD0Entry(
    IN  WDFDEVICE Device,
    IN  WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PPORTS_DEVICE	pContext = GetPortsDevice(Device);
    UNREFERENCED_PARAMETER(PreviousState);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<--> %s\n", __FUNCTION__);

    if(!pContext->DeviceOK)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Setting VIRTIO_CONFIG_S_FAILED flag\n");
        VirtIODeviceAddStatus(&pContext->IODevice, VIRTIO_CONFIG_S_FAILED);
    }
    else
    {
        VIOSerialInitAllQueues(Device);
        VIOSerialRenewAllPorts(Device);
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Setting VIRTIO_CONFIG_S_DRIVER_OK flag\n");
        VirtIODeviceAddStatus(&pContext->IODevice, VIRTIO_CONFIG_S_DRIVER_OK);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
VIOSerialEvtDeviceD0Exit(
    IN  WDFDEVICE Device,
    IN  WDF_POWER_DEVICE_STATE TargetState
    )
{
    UNREFERENCED_PARAMETER(TargetState);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<--> %s\n", __FUNCTION__);

    PAGED_CODE();

    VIOSerialShutdownAllPorts(Device);
    VIOSerialShutDownAllQueues(Device);

    return STATUS_SUCCESS;
}


NTSTATUS
VIOSerialEvtDeviceD0EntryPostInterruptsEnabled(
    IN  WDFDEVICE WdfDevice,
    IN  WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PPORTS_DEVICE	pContext = GetPortsDevice(WdfDevice);
    UNREFERENCED_PARAMETER(PreviousState);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<--> %s\n", __FUNCTION__);

    PAGED_CODE();

    if(!pContext->DeviceOK)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Sending VIRTIO_CONSOLE_DEVICE_READY 0\n");
        VIOSerialSendCtrlMsg(WdfDevice, VIRTIO_CONSOLE_BAD_ID, VIRTIO_CONSOLE_DEVICE_READY, 0);
    }
    else if (PreviousState == WdfPowerDeviceD3Final)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Sending VIRTIO_CONSOLE_DEVICE_READY 1\n");
        VIOSerialSendCtrlMsg(WdfDevice, VIRTIO_CONSOLE_BAD_ID, VIRTIO_CONSOLE_DEVICE_READY, 1);
    }

    return STATUS_SUCCESS;
}

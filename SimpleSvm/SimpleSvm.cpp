/*!
    @file       SimpleSvm.cpp

    @brief      All C code.

    @author     Satoshi Tanda

	@redefine  JJJJJJ      hrbust86@126.com

    @copyright  Copyright (c) 2017-2018, Satoshi Tanda. All rights reserved.
 */

#include "SvmStruct.h"
#include "SvmTraps.h"
#include "SvmUtil.h"
#include "HookSyscall/SvmHookMsr.h"
#include "BaseUtil.h"

EXTERN_C DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD SvDriverUnload;
static CALLBACK_FUNCTION SvPowerCallbackRoutine;

//
// A power state callback handle.
//
static PVOID g_PowerCallbackRegistration;

/*!
    @brief      Allocates page aligned, zero filled physical memory.

    @details    This function allocates page aligned nonpaged pool. The
                allocated memory is zero filled and must be freed with
                SvFreePageAlingedPhysicalMemory. On Windows 8 and later versions
                of Windows, the allocated memory is non executable.

    @param[in]  NumberOfBytes - A size of memory to allocate in byte. This must
                be equal or greater than PAGE_SIZE.

    @result     A pointer to the allocated memory filled with zero; or NULL when
                there is insufficient memory to allocate requested size.
 */
__drv_allocatesMem(Mem)
_Post_writable_byte_size_(NumberOfBytes)
_Post_maybenull_
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
_Must_inspect_result_
static
PVOID
SvAllocatePageAlingedPhysicalMemory (
    _In_ SIZE_T NumberOfBytes
    )
{
    PVOID memory;

    //
    // The size must be equal or greater than PAGE_SIZE in order to allocate
    // page aligned memory.
    //
    NT_ASSERT(NumberOfBytes >= PAGE_SIZE);

#pragma prefast(disable : 28118 __WARNING_ERROR, "FP due to POOL_NX_OPTIN.")
    memory = ExAllocatePoolWithTag(NonPagedPool, NumberOfBytes, 'MVSS');
    if (memory != nullptr)
    {
        NT_ASSERT(PAGE_ALIGN(memory) == memory);
        RtlZeroMemory(memory, NumberOfBytes);
    }
    return memory;
}

/*!
    @brief      Frees memory allocated by SvAllocatePageAlingedPhysicalMemory.

    @param[in]  BaseAddress - The address returned by
                SvAllocatePageAlingedPhysicalMemory.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
static
VOID
SvFreePageAlingedPhysicalMemory (
    _Pre_notnull_ __drv_freesMem(Mem) PVOID BaseAddress
    )
{
    ExFreePoolWithTag(BaseAddress, 'MVSS');
}

/*!
    @brief      Allocates page aligned, zero filled contiguous physical memory.

    @details    This function allocates page aligned nonpaged pool where backed
                by contiguous physical pages. The allocated memory is zero
                filled and must be freed with SvFreeContiguousMemory. The
                allocated memory is executable.

    @param[in]  NumberOfBytes - A size of memory to allocate in byte.

    @result     A pointer to the allocated memory filled with zero; or NULL when
                there is insufficient memory to allocate requested size.
 */
_Post_writable_byte_size_(NumberOfBytes)
_Post_maybenull_
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
_Must_inspect_result_
static
PVOID
SvAllocateContiguousMemory (
    _In_ SIZE_T NumberOfBytes
    )
{
    PVOID memory;
    PHYSICAL_ADDRESS boundary, lowest, highest;

    boundary.QuadPart = lowest.QuadPart = 0;
    highest.QuadPart = -1;

#pragma prefast(disable : 30030, "No alternative API on Windows 7.")
    memory = MmAllocateContiguousMemorySpecifyCacheNode(NumberOfBytes,
                                                        lowest,
                                                        highest,
                                                        boundary,
                                                        MmCached,
                                                        MM_ANY_NODE_OK);
    if (memory != nullptr)
    {
        RtlZeroMemory(memory, NumberOfBytes);
    }
    return memory;
}

/*!
    @brief      Frees memory allocated by SvAllocateContiguousMemory.

    @param[in]  BaseAddress - The address returned by SvAllocateContiguousMemory.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
static
VOID
SvFreeContiguousMemory (
    _In_ PVOID BaseAddress
    )
{
    MmFreeContiguousMemory(BaseAddress);
}

/*!
    @brief          Handles #VMEXIT due to execution of the CPUID instructions.

    @details        This function returns unmodified results of the CPUID
                    instruction, except for few cases to indicate presence of
                    the hypervisor, and to process an unload request.

                    CPUID leaf 0x40000000 and 0x40000001 return modified values
                    to conform to the hypervisor interface to some extent. See
                    "Requirements for implementing the Microsoft Hypervisor interface"
                    https://msdn.microsoft.com/en-us/library/windows/hardware/Dn613994(v=vs.85).aspx
                    for details of the interface.

    @param[inout]   VpData - Per processor data.
    @param[inout]   GuestRegisters - Guest's GPRs.
 */
_IRQL_requires_same_
static
VOID
SvHandleCpuid (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _Inout_ PGUEST_CONTEXT GuestContext
    )
{
    int registers[4];   // EAX, EBX, ECX, and EDX
    int leaf, subLeaf;
    SEGMENT_ATTRIBUTE attribute;

    //
    // Execute CPUID as requested.
    //
    leaf = static_cast<int>(GuestContext->VpRegs->Rax);
    subLeaf = static_cast<int>(GuestContext->VpRegs->Rcx);
    __cpuidex(registers, leaf, subLeaf);

    switch (leaf)
    {
    case CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS:
        //
        // Indicate presence of a hypervisor by setting the bit that are
        // reserved for use by hypervisor to indicate guest status. See "CPUID
        // Fn0000_0001_ECX Feature Identifiers".
        //
        registers[3] |= CPUID_FN0000_0001_ECX_HYPERVISOR_PRESENT;
        break;
    case CPUID_HV_VENDOR_AND_MAX_FUNCTIONS:
        //
        // Return a maximum supported hypervisor CPUID leaf range and a vendor
        // ID signature as required by the spec.
        //
        registers[0] = CPUID_HV_MAX;
        registers[1] = 'NmvS';  // "SvmNest     "
        registers[2] = ' tse';
        registers[3] = '    ';
        break;
    case CPUID_HV_INTERFACE:
        //
        // Return non Hv#1 value. This indicate that the SimpleSvm does NOT
        // conform to the Microsoft hypervisor interface.
        //
        registers[0] = '0#vH';  // Hv#0
        registers[1] = registers[2] = registers[3] = 0;
        break;
    case CPUID_UNLOAD_SIMPLE_SVM:
        if (subLeaf == CPUID_UNLOAD_SIMPLE_SVM)
        {
            //
            // Unload itself if the request is from the kernel mode.
            //
            attribute.AsUInt16 = VpData->GuestVmcb.StateSaveArea.SsAttrib;
            if (attribute.Fields.Dpl == DPL_SYSTEM)
            {
                GuestContext->ExitVm = EXIT_REASON::EXIT_EXIT;
            }
            break;
        }
    default:
        break;
    }

    //
    // Update guest's GPRs with results.
    //
    GuestContext->VpRegs->Rax = registers[0];
    GuestContext->VpRegs->Rbx = registers[1];
    GuestContext->VpRegs->Rcx = registers[2];
    GuestContext->VpRegs->Rdx = registers[3];

    //
    // Debug prints results. Very important to note that any use of API from
    // the host context is unsafe and absolutely avoided, unless the API is
    // documented to be accessible on IRQL IPI_LEVEL+. This is because
    // interrupts are disabled when host code is running, and IPI is not going
    // to be delivered when it is issued.
    //
    // This code is not exception and violating this rule. The reasons for this
    // code are to demonstrate a bad example, and simply show that the SimpleSvm
    // is functioning for a test purpose.
    //
    if (KeGetCurrentIrql() <= DISPATCH_LEVEL)
    {
        SvDebugPrint("[SvmNest] CPUID: %08x-%08x : %08x %08x %08x %08x\n",
                     leaf,
                     subLeaf,
                     registers[0],
                     registers[1],
                     registers[2],
                     registers[3]);
    }

    //
    // Then, advance RIP to "complete" the instruction.
    //
    VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;
	//VpData->GuestVmcb.StateSaveArea.Rip += 2;
}

_IRQL_requires_same_
static
VOID
SvHandleMsrAccess (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _Inout_ PGUEST_CONTEXT GuestContext
    )
{
	auto MsrNum = GuestContext->VpRegs->Rcx;
	switch (MsrNum)
	{
	case IA32_MSR_EFER:
		//SvHandleEFERWrite(VpData, GuestContext);
        SvHandleEffer(VpData, GuestContext);
        break;
	case IA32_MSR_LSTR:
		SvHandleLstrRead(VpData, GuestContext);
		break;
    case IA32_MSR_VM_HSAVE:
        SvHandleSvmHsave(VpData, GuestContext);
        break;
	default:
		SvInjectGeneralProtectionException(VpData);
		break;
	}
}

/*!
    @brief          Handles #VMEXIT due to execution of the VMRUN instruction.

    @details        This function always injects #GP to the guest.

    @param[inout]   VpData - Per processor data.
    @param[inout]   GuestRegisters - Guest's GPRs.
 */
// _IRQL_requires_same_
// static
// VOID
// SvHandleVmrun (
//     _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
//     _Inout_ PGUEST_CONTEXT GuestContext
//     )
// {
//     UNREFERENCED_PARAMETER(GuestContext);
// 
//     SvInjectGeneralProtectionException(VpData);
// }

/*!
    @brief          C-level entry point of the host code called from SvLaunchVm.

    @details        This function loads save host state first, and then, handles
                    #VMEXIT which may or may not change guest's state via VpData
                    or GuestRegisters.

                    Interrupts are disabled when this function is called due to
                    the cleared GIF. Not all host state are loaded yet, so do it
                    with the VMLOAD instruction.

                    If the #VMEXIT handler detects a request to unload the
                    hypervisor, this function loads guest state, disables SVM
                    and returns to execution flow where the #VMEXIT triggered.

    @param[inout]   VpData - Per processor data.
    @param[inout]   GuestRegisters - Guest's GPRs.

    @result         TRUE when virtualization is terminated; otherwise FALSE.
 */
_IRQL_requires_same_
EXTERN_C
long
NTAPI
SvHandleVmExit (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _Inout_ PGUEST_REGISTERS GuestRegisters
    )
{
    GUEST_CONTEXT guestContext;

    //
    // Load some host state that are not loaded on #VMEXIT.
    //
	if (CPU_MODE::VmxMode != VpData->HostStackLayout.pProcessNestData->CpuMode && 
		NULL == VpData->HostStackLayout.pProcessNestData->vcpu_vmx)
	{
		__svm_vmload(VpData->HostStackLayout.HostVmcbPa);
	}
	else
	{
		__svm_vmload(VpData->HostStackLayout.pProcessNestData->vcpu_vmx->vmcb_host_02_pa); // in the nest , we need use
	}

    NT_ASSERT(VpData->HostStackLayout.Reserved1 == MAXUINT64);

    //
    // Handle #VMEXIT according with its reason.
    //
	if (CPU_MODE::VmxMode != VpData->HostStackLayout.pProcessNestData->CpuMode)
	{
		//
		// Guest's RAX is overwritten by the host's value on #VMEXIT and saved in
		// the VMCB instead. Reflect the guest RAX to the context.
		//
		GuestRegisters->Rax = VpData->GuestVmcb.StateSaveArea.Rax;

		guestContext.VpRegs = GuestRegisters;
		guestContext.ExitVm = EXIT_REASON::EXIT_NOTHING;

		switch (VpData->GuestVmcb.ControlArea.ExitCode)
		{
		case VMEXIT_CPUID:
			SvHandleCpuid(VpData, &guestContext);
			break;
		case VMEXIT_MSR:
			SvHandleMsrAccess(VpData, &guestContext);
			break;
		case VMEXIT_VMRUN:
			//SvHandleVmrun(VpData, &guestContext);
			SvHandleVmrunEx(VpData, &guestContext);
			break;
		case VMEXIT_VMMCALL:
			SvHandleVmmcall(VpData, &guestContext);
			break;
		case VMEXIT_NPF:
			SV_DEBUG_BREAK();
			break;
		default:
			SV_DEBUG_BREAK();
#pragma prefast(disable : __WARNING_USE_OTHER_FUNCTION, "Unrecoverble path.")
			KeBugCheck(MANUALLY_INITIATED_CRASH);
		}
	}
	else
	{
		PVMCB pVmcbGuest02va = (PVMCB)UtilVaFromPa(VpData->HostStackLayout.pProcessNestData->vcpu_vmx->vmcb_guest_02_pa);
		UINT64 ullExitCode = pVmcbGuest02va->ControlArea.ExitCode;

		GuestRegisters->Rax = pVmcbGuest02va->StateSaveArea.Rax;

		guestContext.VpRegs = GuestRegisters;
		guestContext.ExitVm = EXIT_REASON::EXIT_NOTHING;

        SV_DEBUG_BREAK();
		switch (ullExitCode)
		{
		case VMEXIT_CPUID:
			SvHandleCpuidForL2ToL1(VpData, &guestContext);
			break;
		case VMEXIT_MSR:
			SvHandleMsrAccessNest(VpData, &guestContext);
			break;
		case VMEXIT_VMRUN:
            SvHandleVmrunExForL1ToL2(VpData, &guestContext);
			break;
		case VMEXIT_VMMCALL:
            SvHandleVmmcallNest(VpData, &guestContext);
			break;
        case VMEXIT_EXCEPTION_BP:
            SvHandleBreakPointExceptionNest(VpData, &guestContext);
            break;
		default:
			SV_DEBUG_BREAK();
#pragma prefast(disable : __WARNING_USE_OTHER_FUNCTION, "Unrecoverble path.")
			KeBugCheck(MANUALLY_INITIATED_CRASH);
		}
	}

    //
    // Terminate the SimpleSvm hypervisor if requested.
    //
    if (EXIT_REASON::EXIT_EXIT == guestContext.ExitVm)
    {
        NT_ASSERT(VpData->GuestVmcb.ControlArea.ExitCode == VMEXIT_CPUID);

        //
        // Set return values of CPUID instruction as follows:
        //  RBX     = An address to return
        //  RCX     = A stack pointer to restore
        //  EDX:EAX = An address of per processor data to be freed by the caller
        //
        guestContext.VpRegs->Rax = reinterpret_cast<UINT64>(VpData) & MAXUINT32;
        guestContext.VpRegs->Rbx = VpData->GuestVmcb.ControlArea.NRip;
        guestContext.VpRegs->Rcx = VpData->GuestVmcb.StateSaveArea.Rsp;
        guestContext.VpRegs->Rdx = reinterpret_cast<UINT64>(VpData) >> 32;

        //
        // Load guest state (currently host state is loaded).
        //
        __svm_vmload(MmGetPhysicalAddress(&VpData->GuestVmcb).QuadPart);

        //
        // Set the global interrupt flag (GIF) but still disable interrupts by
        // clearing IF. GIF must be set to return to the normal execution, but
        // interruptions are not desirable until SVM is disabled as it would
        // execute random kernel-code in the host context.
        //
        _disable();
        __svm_stgi();

        //
        // Disable SVM, and restore the guest RFLAGS. This may enable interrupts.
        // Some of arithmetic flags are destroyed by the subsequent code.
        //
        __writemsr(IA32_MSR_EFER, __readmsr(IA32_MSR_EFER) & ~EFER_SVME);
        __writeeflags(VpData->GuestVmcb.StateSaveArea.Rflags);
        goto Exit;
    }

    //
    // Reflect potentially updated guest's RAX to VMCB. Again, unlike other GPRs,
    // RAX is loaded from VMCB on VMRUN.
    //
	if (CPU_MODE::VmxMode != VpData->HostStackLayout.pProcessNestData->CpuMode)
	{
		VpData->GuestVmcb.StateSaveArea.Rax = guestContext.VpRegs->Rax;
	}
	else
	{
		PVMCB pVmcbGuest02va = (PVMCB)UtilVaFromPa(VpData->HostStackLayout.pProcessNestData->vcpu_vmx->vmcb_guest_02_pa);
		pVmcbGuest02va->StateSaveArea.Rax = guestContext.VpRegs->Rax;
	}

Exit:
    NT_ASSERT(VpData->HostStackLayout.Reserved1 == MAXUINT64);
    return guestContext.ExitVm;
}

/*!
    @brief      Returns attributes of a segment specified by the segment selector.

    @details    This function locates a segment descriptor from the segment
                selector and the GDT base, extracts attributes of the segment,
                and returns it. The returned value is the same as what the "dg"
                command of Windbg shows as "Flags". Here is an example output
                with 0x18 of the selector:
                ----
                0: kd> dg 18
                P Si Gr Pr Lo
                Sel        Base              Limit          Type    l ze an es ng Flags
                ---- ----------------- ----------------- ---------- - -- -- -- -- --------
                0018 00000000`00000000 00000000`00000000 Data RW Ac 0 Bg By P  Nl 00000493
                ----

    @param[in]  SegmentSelector - A segment selector to get attributes of a
                corresponding descriptor.
    @param[in]  GdtBase - A base address of GDT.

    @result     Attributes of the segment.
 */
_IRQL_requires_same_
_Check_return_
static
UINT16
SvGetSegmentAccessRight (
    _In_ UINT16 SegmentSelector,
    _In_ ULONG_PTR GdtBase
    )
{
    PSEGMENT_DESCRIPTOR descriptor;
    SEGMENT_ATTRIBUTE attribute;

    //
    // Get a segment descriptor corresponds to the specified segment selector.
    //
    descriptor = reinterpret_cast<PSEGMENT_DESCRIPTOR>(
                                        GdtBase + (SegmentSelector & ~RPL_MASK));

    //
    // Extract all attribute fields in the segment descriptor to a structure
    // that describes only attributes (as opposed to the segment descriptor
    // consists of multiple other fields).
    //
    attribute.Fields.Type = descriptor->Fields.Type;
    attribute.Fields.System = descriptor->Fields.System;
    attribute.Fields.Dpl = descriptor->Fields.Dpl;
    attribute.Fields.Present = descriptor->Fields.Present;
    attribute.Fields.Avl = descriptor->Fields.Avl;
    attribute.Fields.LongMode = descriptor->Fields.LongMode;
    attribute.Fields.DefaultBit = descriptor->Fields.DefaultBit;
    attribute.Fields.Granularity = descriptor->Fields.Granularity;
    attribute.Fields.Reserved1 = 0;

    return attribute.AsUInt16;
}

/*!
    @brief      Tests whether the SimpleSvm hypervisor is installed.

    @details    This function checks a result of CPUID leaf 40000000h, which
                should return a vendor name of the hypervisor if any of those
                who implement the Microsoft Hypervisor interface is installed.
                If the SimpleSvm hypervisor is installed, this should return
                "SimpleSvm", and if no hypervisor is installed, it the result of
                CPUID is undefined. For more details of the interface, see
                "Requirements for implementing the Microsoft Hypervisor interface"
                https://msdn.microsoft.com/en-us/library/windows/hardware/Dn613994(v=vs.85).aspx

    @result     TRUE when the SimpleSvm is installed; otherwise, FALSE.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
_Check_return_
static
BOOLEAN
SvIsSimpleSvmHypervisorInstalled (
    VOID
    )
{
    int registers[4];   // EAX, EBX, ECX, and EDX
    char vendorId[13];

    //
    // When the SimpleSvm hypervisor is installed, CPUID leaf 40000000h will
    // return "SimpleSvm   " as the vendor name.
    //
    __cpuid(registers, CPUID_HV_VENDOR_AND_MAX_FUNCTIONS);
    RtlCopyMemory(vendorId + 0, &registers[1], sizeof(registers[1]));
    RtlCopyMemory(vendorId + 4, &registers[2], sizeof(registers[2]));
    RtlCopyMemory(vendorId + 8, &registers[3], sizeof(registers[3]));
    vendorId[12] = ANSI_NULL;

    return (strcmp(vendorId, "SvmNest     ") == 0);
}

/*!
    @brief      Virtualize the current processor.

    @details    This function enables SVM, initialize VMCB with the current
                processor state, and enters the guest mode on the current
                processor.

    @param[in]  Context - A pointer of share data.

    @result     STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
static
VOID
SvPrepareForVirtualization (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _In_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ const CONTEXT* ContextRecord
    )
{
    DESCRIPTOR_TABLE_REGISTER gdtr, idtr;
    PHYSICAL_ADDRESS guestVmcbPa, hostVmcbPa, hostStateAreaPa, pml4BasePa, msrpmPa;

    //
    // Capture the current GDTR and IDTR to use as initial values of the guest
    // mode.
    //
    _sgdt(&gdtr);
    __sidt(&idtr);

    guestVmcbPa = MmGetPhysicalAddress(&VpData->GuestVmcb);
    hostVmcbPa = MmGetPhysicalAddress(&VpData->HostVmcb);
    hostStateAreaPa = MmGetPhysicalAddress(&VpData->HostStateArea);
    pml4BasePa = MmGetPhysicalAddress(&SharedVpData->Pml4Entries);
    msrpmPa = MmGetPhysicalAddress(SharedVpData->MsrPermissionsMap);

    VpData->HostStackLayout.pProcessNestData->vcpu_vmx = NULL;
    VpData->HostStackLayout.pProcessNestData->CpuMode = ProtectedMode;
    VpData->HostStackLayout.pProcessNestData->GuestMsrEFER.QuadPart = __readmsr((ULONGLONG)Msr::kIa32Efer);
    VpData->HostStackLayout.pProcessNestData->GuestSvmHsave12.QuadPart = 0;
    //InterlockedIncrement(&VpData->HostStackLayout.pProcessNestData->shared_data->reference_count);

    //
    // Intercept break point exception. This is required to redirect execution
    // of a hooked address (where a breakpoint is set) to the corresponding
    // hook handler function with our hypervisor.
    //
    VpData->GuestVmcb.ControlArea.InterceptException |= (1UL << 3);

    //
    // Configure to trigger #VMEXIT with CPUID and VMRUN instructions. CPUID is
    // intercepted to present existence of the SimpleSvm hypervisor and provide
    // an interface to ask it to unload itself.
    //
    // VMRUN is intercepted because it is required by the processor to enter the
    // guest mode; otherwise, #VMEXIT occurs due to VMEXIT_INVALID when a
    // processor attempts to enter the guest mode. See "Canonicalization and
    // Consistency Checks" on "VMRUN Instruction".
    //
    VpData->GuestVmcb.ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_CPUID;
    VpData->GuestVmcb.ControlArea.InterceptMisc2 |= SVM_INTERCEPT_MISC2_VMRUN;
	VpData->GuestVmcb.ControlArea.InterceptMisc2 |= SVM_INTERCEPT_MISC2_VMMCALL;

    //
    // Also, configure to trigger #VMEXIT on MSR access as configured by the
    // MSRPM. In our case, write to IA32_MSR_EFER is intercepted.
    //
    VpData->GuestVmcb.ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_MSR_PROT;
    VpData->GuestVmcb.ControlArea.MsrpmBasePa = msrpmPa.QuadPart;

    //
    // Specify guest's address space ID (ASID). TLB is maintained by the ID for
    // guests. Use the same value for all processors since all of them run a
    // single guest in our case. Use 1 as the most likely supported ASID by the
    // processor. The actual the supported number of ASID can be obtained with
    // CPUID. See "CPUID Fn8000_000A_EBX SVM Revision and Feature
    // Identification". Zero of ASID is reserved and illegal.
    //
    VpData->GuestVmcb.ControlArea.GuestAsid = 1;

    //
    // Enable Nested Page Tables. By enabling this, the processor performs the
    // nested page walk, that involves with an additional page walk to translate
    // a guest physical address to a system physical address. An address of
    // nested page tables is specified by the NCr3 field of VMCB.
    //
    // We have already build the nested page tables with SvBuildNestedPageTables.
    //
    // Note that our hypervisor does not trigger any additional #VMEXIT due to
    // the use of Nested Page Tables since all physical addresses from 0-512 GB
    // are configured to be accessible from the guest.
    //

    VpData->GuestVmcb.ControlArea.NpEnable |= SVM_NP_ENABLE_NP_ENABLE;
    VpData->GuestVmcb.ControlArea.NCr3 = pml4BasePa.QuadPart;

    VpData->GuestVmcb.ControlArea.LbrVirtualizationEnable |= SVM_ENABLE_NEST_SVM;

    // This feature allows STGI and CLGI to execute in Guest mode and control virtual interrupts in guest 
    // mode while still allowing physical interrupts to be intercepted by the hypervisor. 
    VpData->GuestVmcb.ControlArea.VIntr |= SVM_ENABLE_VIRTUAL_GIF;

    //
    // Set up the initial guest state based on the current system state. Those
    // values are loaded into the processor as guest state when the VMRUN
    // instruction is executed.
    //
    VpData->GuestVmcb.StateSaveArea.GdtrBase = gdtr.Base;
    VpData->GuestVmcb.StateSaveArea.GdtrLimit = gdtr.Limit;
    VpData->GuestVmcb.StateSaveArea.IdtrBase = idtr.Base;
    VpData->GuestVmcb.StateSaveArea.IdtrLimit = idtr.Limit;

    VpData->GuestVmcb.StateSaveArea.CsLimit = GetSegmentLimit(ContextRecord->SegCs);
    VpData->GuestVmcb.StateSaveArea.DsLimit = GetSegmentLimit(ContextRecord->SegDs);
    VpData->GuestVmcb.StateSaveArea.EsLimit = GetSegmentLimit(ContextRecord->SegEs);
    VpData->GuestVmcb.StateSaveArea.SsLimit = GetSegmentLimit(ContextRecord->SegSs);
    VpData->GuestVmcb.StateSaveArea.CsSelector = ContextRecord->SegCs;
    VpData->GuestVmcb.StateSaveArea.DsSelector = ContextRecord->SegDs;
    VpData->GuestVmcb.StateSaveArea.EsSelector = ContextRecord->SegEs;
    VpData->GuestVmcb.StateSaveArea.SsSelector = ContextRecord->SegSs;
    VpData->GuestVmcb.StateSaveArea.CsAttrib = SvGetSegmentAccessRight(ContextRecord->SegCs, gdtr.Base);
    VpData->GuestVmcb.StateSaveArea.DsAttrib = SvGetSegmentAccessRight(ContextRecord->SegDs, gdtr.Base);
    VpData->GuestVmcb.StateSaveArea.EsAttrib = SvGetSegmentAccessRight(ContextRecord->SegEs, gdtr.Base);
    VpData->GuestVmcb.StateSaveArea.SsAttrib = SvGetSegmentAccessRight(ContextRecord->SegSs, gdtr.Base);

    VpData->GuestVmcb.StateSaveArea.Efer = __readmsr(IA32_MSR_EFER);
    VpData->GuestVmcb.StateSaveArea.Cr0 = __readcr0();
    VpData->GuestVmcb.StateSaveArea.Cr2 = __readcr2();
    VpData->GuestVmcb.StateSaveArea.Cr3 = __readcr3();
    VpData->GuestVmcb.StateSaveArea.Cr4 = __readcr4();
    VpData->GuestVmcb.StateSaveArea.Rflags = ContextRecord->EFlags;
    VpData->GuestVmcb.StateSaveArea.Rsp = ContextRecord->Rsp;
    VpData->GuestVmcb.StateSaveArea.Rip = ContextRecord->Rip;
    VpData->GuestVmcb.StateSaveArea.GPat = __readmsr(IA32_MSR_PAT);

    //
    // Save some of the current state on VMCB. Some of those states are:
    // - FS, GS, TR, LDTR (including all hidden state)
    // - KernelGsBase
    // - STAR, LSTAR, CSTAR, SFMASK
    // - SYSENTER_CS, SYSENTER_ESP, SYSENTER_EIP
    // See "VMSAVE and VMLOAD Instructions" for mode details.
    //
    // Those are restored to the processor right before #VMEXIT with the VMLOAD
    // instruction so that the guest can start its execution with saved state,
    // and also, re-saved to the VMCS with right after #VMEXIT with the VMSAVE
    // instruction so that the host (hypervisor) do not destroy guest's state.
    //
    __svm_vmsave(guestVmcbPa.QuadPart);

    //
    // Store data to stack so that the host (hypervisor) can use those values.
    //
    VpData->HostStackLayout.Reserved1 = MAXUINT64;
	//VpData->HostStackLayout.OriginalMsrLstar = 0;
    VpData->HostStackLayout.SharedVpData = SharedVpData;
    VpData->HostStackLayout.Self = VpData;
    VpData->HostStackLayout.HostVmcbPa = hostVmcbPa.QuadPart;
    VpData->HostStackLayout.GuestVmcbPa = guestVmcbPa.QuadPart;

    //
    // Set an address of the host state area to VM_HSAVE_PA MSR. The processor
    // saves some of the current state on VMRUN and loads them on #VMEXIT. See
    // "VM_HSAVE_PA MSR (C001_0117h)".
    //
    __writemsr(IA32_MSR_VM_HSAVE, hostStateAreaPa.QuadPart);

    //
    // Also, save some of the current state to VMCB for the host. This is loaded
    // after #VMEXIT to reproduce the current state for the host (hypervisor).
    //
    __svm_vmsave(hostVmcbPa.QuadPart);
}

/*!
    @brief      Virtualize the current processor.

    @details    This function enables SVM, initialize VMCB with the current
                processor state, and enters the guest mode on the current
                processor.

    @param[in]  Context - A pointer of share data.

    @result     STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
_Check_return_
static
NTSTATUS
SvVirtualizeProcessor (
    _In_opt_ PVOID Context
    )
{
    NTSTATUS status;
    PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData;
    PVIRTUAL_PROCESSOR_DATA vpData;
    CONTEXT contextRecord;

    SV_DEBUG_BREAK();

    vpData = nullptr;

    if (!ARGUMENT_PRESENT(Context))
    {
        status = STATUS_INVALID_PARAMETER_1;
        goto Exit;
    }

    //
    // Allocate per processor data.
    //
#pragma prefast(push)
#pragma prefast(disable : __WARNING_MEMORY_LEAK, "Ownership is taken on success.")
    vpData = reinterpret_cast<PVIRTUAL_PROCESSOR_DATA>(
            SvAllocatePageAlingedPhysicalMemory(sizeof(VIRTUAL_PROCESSOR_DATA)));
#pragma prefast(pop)
    if (vpData == nullptr)
    {
        SvDebugPrint("[SvmNest] Insufficient memory.\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    vpData->HostStackLayout.pProcessNestData = reinterpret_cast<ProcessorNestData *>
        (SvAllocatePageAlingedPhysicalMemory(PAGE_SIZE));
    
    if (nullptr == vpData->HostStackLayout.pProcessNestData)
    {
        SvDebugPrint("[SvmNest] error ProcessNestData");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Capture the current RIP, RSP, RFLAGS, and segment selectors. This
    // captured state is used as an initial state of the guest mode; therefore
    // when virtualization starts by the later call of SvLaunchVm, a processor
    // resume its execution at this location and state.
    //
    RtlCaptureContext(&contextRecord);

    //
    // First time of this execution, the SimpleSvm hypervisor is not installed
    // yet. Therefore, the branch is taken, and virtualization is attempted.
    //
    // At the second execution of here, after SvLaunchVm virtualized the
    // processor, SvIsSimpleSvmHypervisorInstalled returns TRUE, and this
    // function exits with STATUS_SUCCESS.
    //
    if (SvIsSimpleSvmHypervisorInstalled() == FALSE)
    {
        SvDebugPrint("[SvmNest] Attempting to virtualize the processor.\n");
        sharedVpData = reinterpret_cast<PSHARED_VIRTUAL_PROCESSOR_DATA>(Context);

	  //
	  // Enable SVM by setting EFER.SVME. It has already been verified that this
	  // bit was writable with SvIsSvmSupported.
	  //
		__writemsr(IA32_MSR_EFER, __readmsr(IA32_MSR_EFER) | EFER_SVME);

        //
        // Set up VMCB, the structure describes the guest state and what events
        // within the guest should be intercepted, ie, triggers #VMEXIT.
        //
        SvPrepareForVirtualization(vpData, sharedVpData, &contextRecord);

        //
        // Switch to the host RSP to run as the host (hypervisor), and then
        // enters loop that executes code as a guest until #VMEXIT happens and
        // handles #VMEXIT as the host.
        //
        // This function should never return to here.
        //
        SvLaunchVm(&vpData->HostStackLayout.GuestVmcbPa);
        SV_DEBUG_BREAK();
        KeBugCheck(MANUALLY_INITIATED_CRASH);
    }

    SvDebugPrint("[SvmNest] The processor has been virtualized.\n");
    status = STATUS_SUCCESS;

Exit:
    if ((!NT_SUCCESS(status)) && (vpData != nullptr))
    {
        //
        // Frees per processor data if allocated and this function is
        // unsuccessful.
        //
        SvFreePageAlingedPhysicalMemory(vpData);
    }
    return status;
}

/*!
    @brief      Execute a callback on all processors one-by-one.

    @details    This function execute Callback with Context as a parameter for
                each processor on the current IRQL. If the callback returned
                non-STATUS_SUCCESS value or any error occurred, this function
                stops execution of the callback and returns the error code.

                When NumOfProcessorCompleted is not NULL, this function always
                set a number of processors that successfully executed the
                callback.

    @param[in]  Callback - A function to execute on all processors.
    @param[in]  Context - A parameter to pass to the callback.
    @param[out] NumOfProcessorCompleted - A pointer to receive a number of
                processors executed the callback successfully.

    @result     STATUS_SUCCESS when Callback executed and returned STATUS_SUCCESS
	            on all processors; otherwise, an appropriate error code.
 */
_IRQL_requires_max_(APC_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
_Check_return_
static
NTSTATUS
SvExecuteOnEachProcessor (
    _In_ NTSTATUS (*Callback)(PVOID),
    _In_opt_ PVOID Context,
    _Out_opt_ PULONG NumOfProcessorCompleted
    )
{
    NTSTATUS status;
    ULONG i, numOfProcessors;
    PROCESSOR_NUMBER processorNumber;
    GROUP_AFFINITY affinity, oldAffinity;

    status = STATUS_SUCCESS;

    //
    // Get a number of processors on this system.
    //
    numOfProcessors = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

    for (i = 0; i < numOfProcessors; i++)
    {
        //
        // Convert from an index to a processor number.
        //
        status = KeGetProcessorNumberFromIndex(i, &processorNumber);
        if (!NT_SUCCESS(status))
        {
            goto Exit;
        }

        //
        // Switch execution of this code to a processor #i.
        //
        affinity.Group = processorNumber.Group;
        affinity.Mask = 1ULL << processorNumber.Number;
        affinity.Reserved[0] = affinity.Reserved[1] = affinity.Reserved[2] = 0;
        KeSetSystemGroupAffinityThread(&affinity, &oldAffinity);

        //
        // Execute the callback.
        //
        status = Callback(Context);

        //
        // Revert the previously executed processor.
        //
        KeRevertToUserGroupAffinityThread(&oldAffinity);

        //
        // Exit if the callback returned error.
        //
        if (!NT_SUCCESS(status))
        {
            goto Exit;
        }
    }

Exit:
    //
    // i must be the same as the number of processors on the system when this
    // function returns STATUS_SUCCESS;
    //
    NT_ASSERT(!NT_SUCCESS(status) || (i == numOfProcessors));

    //
    // Set a number of processors that successfully executed callback if the
    // out parameter is present.
    //
    if (ARGUMENT_PRESENT(NumOfProcessorCompleted))
    {
        *NumOfProcessorCompleted = i;
    }
    return status;
}

/*!
    @brief      De-virtualize the current processor if virtualized.

    @details    This function asks SimpleSVM hypervisor to deactivate itself
                through CPUID with a back-door function id and frees per
                processor data if it is returned. If the SimpleSvm is not
                installed, this function does nothing.

    @param[in]  Context - An out pointer to receive an address of shared data.

    @result     Always STATUS_SUCCESS.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
_Check_return_
static
NTSTATUS
SvDevirtualizeProcessor (
    _In_opt_ PVOID Context
    )
{
    int registers[4];   // EAX, EBX, ECX, and EDX
    UINT64 high, low;
    PVIRTUAL_PROCESSOR_DATA vpData;
    PSHARED_VIRTUAL_PROCESSOR_DATA* sharedVpDataPtr;

    if (!ARGUMENT_PRESENT(Context))
    {
        goto Exit;
    }

    //
    // Ask SimpleSVM hypervisor to deactivate itself. If the hypervisor is
    // installed, this ECX is set to 'JSVM', and EDX:EAX indicates an address
    // of per processor data to be freed.
    //
    __cpuidex(registers, CPUID_UNLOAD_SIMPLE_SVM, CPUID_UNLOAD_SIMPLE_SVM);
    if (registers[2] != 'JSVM')
    {
        goto Exit;
    }

    SvDebugPrint("[SvmNest] The processor has been de-virtualized.\n");

    //
    // Get an address of per processor data indicated by EDX:EAX.
    //
    high = registers[3];
    low = registers[0] & MAXUINT32;
    vpData = reinterpret_cast<PVIRTUAL_PROCESSOR_DATA>(high << 32 | low);
    NT_ASSERT(vpData->HostStackLayout.Reserved1 == MAXUINT64);

    //
    // Save an address of shared data, then free per processor data.
    //
    sharedVpDataPtr = reinterpret_cast<PSHARED_VIRTUAL_PROCESSOR_DATA*>(Context);
    *sharedVpDataPtr = vpData->HostStackLayout.SharedVpData;
    
    if (vpData->HostStackLayout.pProcessNestData)
    {
        SvFreePageAlingedPhysicalMemory(vpData->HostStackLayout.pProcessNestData);
        vpData->HostStackLayout.pProcessNestData = NULL;
    }
    SvFreePageAlingedPhysicalMemory(vpData);

Exit:
    return STATUS_SUCCESS;
}

/*!
    @brief      De-virtualize all virtualized processors.

    @details    This function execute a callback to de-virtualize a processor on
                all processors, and frees shared data when the callback returned
                its pointer from a hypervisor.
 */
_IRQL_requires_max_(APC_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
VOID
SvDevirtualizeAllProcessors (
    VOID
    )
{
    PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData;

    sharedVpData = nullptr;

    //
    // De-virtualize all processors and free shared data when returned.
    //
    NT_VERIFY(NT_SUCCESS(SvExecuteOnEachProcessor(SvDevirtualizeProcessor,
                                                  &sharedVpData,
                                                  nullptr)));
    if (sharedVpData != nullptr)
    {
        SvFreeContiguousMemory(sharedVpData->MsrPermissionsMap);
        SvFreePageAlingedPhysicalMemory(sharedVpData);
    }
}

/*!
    @brief          Build the MSR permissions map (MSRPM).

    @details        This function sets up MSRPM to intercept to IA32_MSR_EFER,
                    as suggested in "Extended Feature Enable Register (EFER)"
                    ----
                    Secure Virtual Machine Enable (SVME) Bit
                    Bit 12, read/write. Enables the SVM extensions. (...) The
                    effect of turning off EFER.SVME while a guest is running is
                    undefined; therefore, the VMM should always prevent guests
                    from writing EFER.
                    ----

                    Each MSR is controlled by two bits in the MSRPM. The LSB of
                    the two bits controls read access to the MSR and the MSB
                    controls write access. A value of 1 indicates that the
                    operation is intercepted. This function locates an offset for
                    IA32_MSR_EFER and sets the MSB bit. For details of logic, see
                    "MSR Intercepts".

    @param[inout]   MsrPermissionsMap - The MSRPM to set up.
 */
_IRQL_requires_same_
static
VOID
SvBuildMsrPermissionsMap (
    _Inout_ PVOID MsrPermissionsMap
    )
{
	// 		MSRPM Byte Offset					MSR Range 
// 			000h�C7FFh								0000_0000h�C0000_1FFFh 
// 			800h�CFFFh								C000_0000h�CC000_1FFFh 
// 			1000h�C17FFh							C001_0000h�CC001_1FFFh 
// 			1800h�C1FFFh							Reserved

    static const UINT32 BITS_PER_MSR = 2;
	static const UINT32 FIRST_MSR_RANGE_BASE = 0x00000000;
    static const UINT32 SECOND_MSR_RANGE_BASE = 0xc0000000;
	static const UINT32 THIRD_MSR_RANGE_BASE = 0xC0010000;
	static const UINT32 FIRST_MSRPM_OFFSET = 0x000 * CHAR_BIT;
    static const UINT32 SECOND_MSRPM_OFFSET = 0x800 * CHAR_BIT;
	static const UINT32 THIRD_MSRPM_OFFSET = 0x1000 * CHAR_BIT;
    RTL_BITMAP bitmapHeader;
    ULONG offsetFromBase, offset;

    //
    // Setup and clear all bits, indicating no MSR access should be intercepted.
    //
    RtlInitializeBitMap(&bitmapHeader,
                        reinterpret_cast<PULONG>(MsrPermissionsMap),
                        SVM_MSR_PERMISSIONS_MAP_SIZE * CHAR_BIT
                        );
    RtlClearAllBits(&bitmapHeader);

    //
    // Compute an offset from the second MSR permissions map offset (0x800) for
    // IA32_MSR_EFER in bits. Then, add an offset until the second MSR
    // permissions map.
    //
    offsetFromBase = (IA32_MSR_EFER - SECOND_MSR_RANGE_BASE) * BITS_PER_MSR;
    offset = SECOND_MSRPM_OFFSET + offsetFromBase;

    //
    // Set the MSB bit indicating write accesses to the MSR should be intercepted.
    //
    RtlSetBits(&bitmapHeader, offset + 1, 1);

	// compute the offset IA32_MSR_LSTR 
    offsetFromBase = (IA32_MSR_LSTR - SECOND_MSR_RANGE_BASE) * BITS_PER_MSR;
	offset = SECOND_MSRPM_OFFSET + offsetFromBase;

	// intercept read and write
	RtlSetBits(&bitmapHeader, offset, 1);
	RtlSetBits(&bitmapHeader, offset + 1, 1);

    // compute the offset  kIa32svmHsave , for nest svm , because this reg would store some key data 
    //The 64 - bit read / write VM_HSAVE_PA MSR holds the physical address of a 4KB block of memory
    //where VMRUN saves host state, and from which #VMEXIT reloads host state.The VMM software is
    //expected to set up this register before issuing the first VMRUN instruction.Software must not attempt
    //to read or write the host save - state area directly.

    offsetFromBase = ((UINT32)Msr::kIa32svmHsave - THIRD_MSR_RANGE_BASE) * BITS_PER_MSR;
    offset = THIRD_MSRPM_OFFSET + offsetFromBase;

    RtlSetBits(&bitmapHeader, offset, 1);
    RtlSetBits(&bitmapHeader, offset + 1, 1);

	// 			000h�C7FFh								0000_0000h�C0000_1FFFh 
// 	for (ULONG i = 0; i <= 0x1FFF; i++)
// 	{
// 		offsetFrom2ndBase = (i - FIRST_MSR_RANGE_BASE) * BITS_PER_MSR;
// 		offset = FIRST_MSRPM_OFFSET + offsetFrom2ndBase;
// 		RtlSetBits(&bitmapHeader, offset, 1); // read
// 		RtlSetBits(&bitmapHeader, offset + 1, 1); // write
// 	}

	// 			800h�CFFFh								C000_0000h�CC000_1FFFh 
// 	for (ULONG i = 0xC0000000; i <= 0xC0001FFF; i++)
// 	{
// 		offsetFrom2ndBase = (i - SECOND_MSR_RANGE_BASE) * BITS_PER_MSR;
// 		offset = SECOND_MSRPM_OFFSET + offsetFrom2ndBase;
// 		RtlSetBits(&bitmapHeader, offset, 1); // read 
// 		RtlSetBits(&bitmapHeader, offset + 1, 1); // write
// 	}

	// 			1000h�C17FFh							C001_0000h�CC001_1FFFh 
// 	for (ULONG i = 0xC0010000; i<= 0xC0011FFF; i++)
// 	{
// 		offsetFrom2ndBase = (i - THIRD_MSR_RANGE_BASE) * BITS_PER_MSR;
// 		offset = THIRD_MSRPM_OFFSET + offsetFrom2ndBase;
// 		RtlSetBits(&bitmapHeader, offset, 1); // read 
// 		RtlSetBits(&bitmapHeader, offset + 1, 1); // write
// 	}

}

/*!
    @brief      Build pass-through style page tables used in nested paging.

    @details    This function build page tables used in Nested Page Tables. The
                page tables are used to translate from a guest physical address
                to a system physical address and pointed by the NCr3 field of
                VMCB, like the traditional page tables are pointed by CR3.

                The nested page tables built in this function are set to
                translate a guest physical address to the same system physical
                address. For example, guest physical address 0x1000 is
                translated into system physical address 0x1000.

                In order to save memory to build nested page tables, 2MB large
                pages are used (as opposed to the standard pages that describe
                translation only for 4K granularity. Also, only up to 512 GB of
                translation is built. 1GB huge pages are not used due to VMware
                not supporting this feature.

    @param[out] SharedVpData - Out buffer to build nested page tables.
 */
_IRQL_requires_same_
static
VOID
SvBuildNestedPageTables (
    _Out_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData
    )
{
    ULONG64 pdpBasePa, pdeBasePa, translationPa;

    //
    // Build only one PML4 entry. This entry has subtables that control up to
    // 512GB physical memory. PFN points to a base physical address of the page
    // directory pointer table.
    //
    pdpBasePa = MmGetPhysicalAddress(&SharedVpData->PdpEntries).QuadPart;
    SharedVpData->Pml4Entries[0].Fields.PageFrameNumber = pdpBasePa >> PAGE_SHIFT;

    //
    // The US (User) bit of all nested page table entries to be translated
    // without #VMEXIT, as all guest accesses are treated as user accesses at
    // the nested level. Also, the RW (Write) bit of nested page table entries
    // that corresponds to guest page tables must be 1 since all guest page
    // table accesses are threated as write access. See "Nested versus Guest
    // Page Faults, Fault Ordering" for more details.
    //
    // Nested page tables built here set 1 to those bits for all entries, so
    // that all translation can complete without triggering #VMEXIT. This does
    // not lower security since security checks are done twice independently:
    // based on guest page tables, and nested page tables. See "Nested versus
    // Guest Page Faults, Fault Ordering" for more details.
    //
    SharedVpData->Pml4Entries[0].Fields.Valid = 1;
    SharedVpData->Pml4Entries[0].Fields.Write = 1;
    SharedVpData->Pml4Entries[0].Fields.User = 1;

    //
    // One PML4 entry controls 512 page directory pointer entires.
    //
    for (ULONG64 i = 0; i < 512; i++)
    {
        //
        // PFN points to a base physical address of the page directory table.
        //
        pdeBasePa = MmGetPhysicalAddress(&SharedVpData->PdeEntries[i][0]).QuadPart;
        SharedVpData->PdpEntries[i].Fields.PageFrameNumber = pdeBasePa >> PAGE_SHIFT;
        SharedVpData->PdpEntries[i].Fields.Valid = 1;
        SharedVpData->PdpEntries[i].Fields.Write = 1;
        SharedVpData->PdpEntries[i].Fields.User = 1;

        //
        // One page directory entry controls 512 page directory entries.
        //
        for (ULONG64 j = 0; j < 512; j++)
        {
            //
            // PFN points to a base physical address of system physical address
            // to be translated from a guest physical address. Set the PS
            // (LargePage) bit to indicate that this is a large page and no
            // subtable exists.
            //
            translationPa = (i * 512) + j;
            SharedVpData->PdeEntries[i][j].Fields.PageFrameNumber = translationPa;
            SharedVpData->PdeEntries[i][j].Fields.Valid = 1;
            SharedVpData->PdeEntries[i][j].Fields.Write = 1;
            SharedVpData->PdeEntries[i][j].Fields.User = 1;
            SharedVpData->PdeEntries[i][j].Fields.LargePage = 1;
        }
    }
}

/*!
    @brief      Test whether the current processor support the SVM feature.

    @details    This function tests whether the current processor has enough
                features to run SimpleSvm, especially about SVM features.

    @result     TRUE if the processor supports the SVM feature; otherwise, FALSE.
 */
_IRQL_requires_same_
_Check_return_
static
BOOLEAN
SvIsSvmSupported (
    VOID
    )
{
    BOOLEAN svmSupported;
    int registers[4];   // EAX, EBX, ECX, and EDX
    ULONG64 vmcr;

    svmSupported = FALSE;

    //
    // Test if the current processor is AMD one. An AMD processor should return
    // "AuthenticAMD" from CPUID function 0. See "Function 0h�Maximum Standard
    // Function Number and Vendor String".
    //
    __cpuid(registers, CPUID_MAX_STANDARD_FN_NUMBER_AND_VENDOR_STRING);
    if ((registers[1] != 'htuA') ||
        (registers[3] != 'itne') ||
        (registers[2] != 'DMAc'))
    {
        goto Exit;
    }

    //
    // Test if the SVM feature is supported by the current processor. See
    // "Enabling SVM" and "CPUID Fn8000_0001_ECX Feature Identifiers".
    //
    __cpuid(registers, CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS_EX);
    if ((registers[2] & CPUID_FN8000_0001_ECX_SVM) == 0)
    {
        goto Exit;
    }

    //
    // Test if the Nested Page Tables feature is supported by the current
    // processor. See "Enabling Nested Paging" and "CPUID Fn8000_000A_EDX SVM
    // Feature Identification".
    //
    __cpuid(registers, CPUID_SVM_FEATURES);
    if ((registers[3] & CPUID_FN8000_000A_EDX_NP) == 0)
    {
        goto Exit;
    }

    //
    // Test if the SVM feature can be enabled. When VM_CR.SVMDIS is set,
    // EFER.SVME cannot be 1; therefore, SVM cannot be enabled. When
    // VM_CR.SVMDIS is clear, EFER.SVME can be written normally and SVM can be
    // enabled. See "Enabling SVM".
    //
    vmcr = __readmsr(SVM_MSR_VM_CR);
    if ((vmcr & SVM_VM_CR_SVMDIS) != 0)
    {
        goto Exit;
    }

    svmSupported = TRUE;

Exit:
    return svmSupported;
}

/*!
    @brief      Virtualizes all processors on the system.

    @details    This function attempts to virtualize all processors on the
                system, and returns STATUS_SUCCESS if all processors are
                successfully virtualized. If any processor is not virtualized,
                this function de-virtualizes all processors and returns an error
                code.

    @result     STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
_IRQL_requires_max_(APC_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
_Check_return_
NTSTATUS
SvVirtualizeAllProcessors (
    VOID
    )
{
    NTSTATUS status;
    PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData;
    ULONG numOfProcessorsCompleted;

    sharedVpData = nullptr;
    numOfProcessorsCompleted = 0;

    //
    // Test whether the current processor supports all required SVM features. If
    // not, exit as error.
    //
    if (SvIsSvmSupported() == FALSE)
    {
        SvDebugPrint("[SvmNest] SVM is not fully supported on this processor.\n");
        status = STATUS_HV_FEATURE_UNAVAILABLE;
        goto Exit;
    }

    //
    // Allocate a data structure shared across all processors. This data is
    // page tables used for Nested Page Tables.
    //
#pragma prefast(push)
#pragma prefast(disable : __WARNING_MEMORY_LEAK, "Ownership is taken on success.")
    sharedVpData = reinterpret_cast<PSHARED_VIRTUAL_PROCESSOR_DATA>(
        SvAllocatePageAlingedPhysicalMemory(sizeof(SHARED_VIRTUAL_PROCESSOR_DATA)));
#pragma prefast(pop)
    if (sharedVpData == nullptr)
    {
        SvDebugPrint("[SvmNest] Insufficient memory.\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Allocate MSR permissions map (MSRPM) onto contiguous physical memory.
    //
    sharedVpData->MsrPermissionsMap = SvAllocateContiguousMemory(
                                                    SVM_MSR_PERMISSIONS_MAP_SIZE);
    if (sharedVpData->MsrPermissionsMap == nullptr)
    {
        SvDebugPrint("[SvmNest] Insufficient memory.\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Build nested page table and MSRPM.
    //
    SvBuildNestedPageTables(sharedVpData);
    SvBuildMsrPermissionsMap(sharedVpData->MsrPermissionsMap);

    //
    // Execute SvVirtualizeProcessor on and virtualize each processor one-by-one.
    // How many processors were successfully virtualized is stored in the third
    // parameter.
    //
    // STATUS_SUCCESS is returned if all processor are successfully virtualized.
    // When any error occurs while virtualizing processors, this function does
    // not attempt to virtualize the rest of processor. Therefore, only part of
    // processors on the system may have been virtualized on error. In this case,
    // it is a caller's responsibility to clean-up (de-virtualize) such
    // processors.
    //
    status = SvExecuteOnEachProcessor(SvVirtualizeProcessor,
                                      sharedVpData,
                                      &numOfProcessorsCompleted);

Exit:
    if (!NT_SUCCESS(status))
    {
        //
        // On failure, after successful allocation of shared data.
        //
        if (numOfProcessorsCompleted != 0)
        {
            //
            // If one or more processors have already been virtualized,
            // de-virtualize any of those processors, and free shared data.
            //
            NT_ASSERT(sharedVpData != nullptr);
            SvDevirtualizeAllProcessors();
        }
        else
        {
            //
            // If none of processors has not been virtualized, simply free
            // shared data.
            //
            if (sharedVpData != nullptr)
            {
                if (sharedVpData->MsrPermissionsMap != nullptr)
                {
                    SvFreeContiguousMemory(sharedVpData->MsrPermissionsMap);
                }
                SvFreePageAlingedPhysicalMemory(sharedVpData);
            }
        }
    }
    return status;
}

/*!
    @brief      An entry point of this driver.

    @param[in]  DriverObject - A driver object.
    @param[in]  RegistryPath - Unused.

    @result     STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
_Use_decl_annotations_
EXTERN_C
NTSTATUS
DriverEntry (
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    UNICODE_STRING objectName;
    OBJECT_ATTRIBUTES objectAttributes;
    PCALLBACK_OBJECT callbackObject;
    PVOID callbackRegistration;

    UNREFERENCED_PARAMETER(RegistryPath);

    SV_DEBUG_BREAK();

    callbackRegistration = nullptr;
    DriverObject->DriverUnload = SvDriverUnload;

    //
    // Opts-in no-execute (NX) nonpaged pool when available for security. By
    // defining POOL_NX_OPTIN as 1 and calling this function, nonpaged pool
    // allocation by the ExAllocatePool family with the NonPagedPool flag
    // automatically allocates NX nonpaged pool on Windows 8 and later versions
    // of Windows, while on Windows 7 where NX nonpaged pool is unsupported,
    // executable nonpaged pool is returned as usual.
    //
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    //
    // Registers a power state callback (SvPowerCallbackRoutine) to handle
    // system sleep and resume to manage virtualization state.
    //
    // First, opens the \Callback\PowerState callback object provides
    // notification regarding power state changes. This is a system defined
    // callback object that was already created by Windows. To open a system
    // defined callback object, the Create parameter of ExCreateCallback must be
    // FALSE (and AllowMultipleCallbacks is ignore when the Create parameter is
    // FALSE).
    //
    objectName = RTL_CONSTANT_STRING(L"\\Callback\\PowerState");
    objectAttributes = RTL_CONSTANT_OBJECT_ATTRIBUTES(&objectName,
                                                      OBJ_CASE_INSENSITIVE);
    status = ExCreateCallback(&callbackObject, &objectAttributes, FALSE, TRUE);
    if (!NT_SUCCESS(status))
    {
        SvDebugPrint("[SvmNest] Failed to open the power state callback object.\n");
        goto Exit;
    }

    //
    // Then, registers our callback. The open callback object must be
    // dereferenced.
    //
    callbackRegistration = ExRegisterCallback(callbackObject,
                                              SvPowerCallbackRoutine,
                                              nullptr);
    ObDereferenceObject(callbackObject);
    if (callbackRegistration == nullptr)
    {
        SvDebugPrint("[SvmNest] Failed to register a power state callback.\n");
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    //
    // Virtualize all processors on the system.
    //
    //status = SvVirtualizeAllProcessors();
	status = StartAmdSvmAndHookMsr();

Exit:
    if (NT_SUCCESS(status))
    {
        //
        // On success, save the registration handle for un-registration.
        //
        NT_ASSERT(callbackRegistration);
        g_PowerCallbackRegistration = callbackRegistration;
    }
    else
    {
        //
        // On any failure, clean up stuff as needed.
        //
        if (callbackRegistration != nullptr)
        {
            ExUnregisterCallback(callbackRegistration);
        }
    }
    return status;
}

/*!
    @brief      Driver unload callback.

    @details    This function de-virtualize all processors on the system.

    @param[in]  DriverObject - Unused.
 */
_Use_decl_annotations_
static
VOID
SvDriverUnload (
    PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    SV_DEBUG_BREAK();

    //
    // Unregister the power state callback.
    //
    NT_ASSERT(g_PowerCallbackRegistration);
    ExUnregisterCallback(g_PowerCallbackRegistration);

    //
    // De-virtualize all processors on the system.
    //
    //SvDevirtualizeAllProcessors();
	StopAmdSvm();
}

/*!
    @brief      PowerState callback routine.

    @details    This function de-virtualize all processors when the system is
                exiting system power state S0 (ie, the system is about to sleep
                etc), and virtualize all processors when the system has just
                reentered S0 (ie, the system has resume from sleep etc).

                Those operations are required because virtualization is cleared
                during sleep.

                For the meanings of parameters, see ExRegisterCallback in MSDN.

    @param[in]  CallbackContext - Unused.
    @param[in]  Argument1 - A PO_CB_XXX constant value.
    @param[in]  Argument2 - A value of TRUE or FALSE.
 */
_Use_decl_annotations_
static
VOID
SvPowerCallbackRoutine (
    PVOID CallbackContext,
    PVOID Argument1,
    PVOID Argument2
    )
{
    UNREFERENCED_PARAMETER(CallbackContext);

    //
    // PO_CB_SYSTEM_STATE_LOCK of Argument1 indicates that a system power state
    // change is imminent.
    //
    if (Argument1 != reinterpret_cast<PVOID>(PO_CB_SYSTEM_STATE_LOCK))
    {
        goto Exit;
    }

    if (Argument2 != FALSE)
    {
        //
        // The system has just reentered S0. Re-virtualize all processors.
        //
        NT_VERIFY(NT_SUCCESS(SvVirtualizeAllProcessors()));
    }
    else
    {
        //
        // The system is about to exit system power state S0. De-virtualize all
        // processors.
        //
        SvDevirtualizeAllProcessors();
    }

Exit:
    return;
}

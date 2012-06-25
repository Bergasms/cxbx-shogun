// ******************************************************************
// *
// *    .,-:::::    .,::      .::::::::.    .,::      .:
// *  ,;;;'````'    `;;;,  .,;;  ;;;'';;'   `;;;,  .,;;
// *  [[[             '[[,,[['   [[[__[[\.    '[[,,[['
// *  $$$              Y$$$P     $$""""Y$$     Y$$$P
// *  `88bo,__,o,    oP"``"Yo,  _88o,,od8P   oP"``"Yo,
// *    "YUMMMMMP",m"       "Mm,""YUMMMP" ,m"       "Mm,
// *
// *   Cxbx->Win32->CxbxKrnl->EmuFS.cpp
// *
// *  This file is part of the Cxbx project.
// *
// *  Cxbx and Cxbe are free software; you can redistribute them
// *  and/or modify them under the terms of the GNU General Public
// *  License as published by the Free Software Foundation; either
// *  version 2 of the license, or (at your option) any later version.
// *
// *  This program is distributed in the hope that it will be useful,
// *  but WITHOUT ANY WARRANTY; without even the implied warranty of
// *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// *  GNU General Public License for more details.
// *
// *  You should have recieved a copy of the GNU General Public License
// *  along with this program; see the file COPYING.
// *  If not, write to the Free Software Foundation, Inc.,
// *  59 Temple Place - Suite 330, Bostom, MA 02111-1307, USA.
// *
// *  (c) 2002-2003 Aaron Robinson <caustik@caustik.com>
// *
// *  All rights reserved
// *
// ******************************************************************
#define _CXBXKRNL_INTERNAL
#define _XBOXKRNL_DEFEXTRN_

// prevent name collisions
namespace xboxkrnl
{
    #include <xboxkrnl/xboxkrnl.h>
};

#include "EmuFS.h"
#include "EmuLDT.h"
#include "EmuAlloc.h"

#undef FIELD_OFFSET     // prevent macro redefinition warnings
#include <windows.h>
#include <cstdio>

// automatically insert after this many EmuFS() swaps
uint32 EmuAutoSleepRate = -1;

// initialize fs segment selector emulation
void EmuInitFS()
{
    EmuInitLDT();
}

// generate fs segment selector
void EmuGenerateFS(Xbe::TLS *pTLS, void *pTLSData)
{
    NT_TIB         *OrgNtTib;
    xboxkrnl::KPCR *NewPcr;

    uint08 *pNewTLS = NULL;

    uint16 NewFS = -1, OrgFS = -1;

    // copy global TLS to the current thread
    {
        uint32 dwCopySize = pTLS->dwDataEndAddr - pTLS->dwDataStartAddr;
        uint32 dwZeroSize = pTLS->dwSizeofZeroFill;

        pNewTLS = (uint08*)CxbxMalloc(dwCopySize + dwZeroSize + 0x100 /* + HACK: extra safety padding 0x100*/);

        memset(pNewTLS, 0, dwCopySize + dwZeroSize + 0x100);
        memcpy(pNewTLS, pTLSData, dwCopySize);
    }

    // dump raw TLS data
    {
        #ifdef _DEBUG_TRACE
        if(pNewTLS == 0)
        {
            DbgPrintf("EmuFS (0x%X): TLS Non-Existant (OK)\n", GetCurrentThreadId());
        }
        else
        {
            DbgPrintf("EmuFS (0x%X): TLS Data Dump...\n", GetCurrentThreadId());
            DbgPrintf("EmuFS (0x%X): 0x%.08X: ", GetCurrentThreadId(), pNewTLS);

            uint32 stop = pTLS->dwDataEndAddr - pTLS->dwDataStartAddr + pTLS->dwSizeofZeroFill;

            for(uint32 v=0;v<stop;v++)
            {
                uint08 *bByte = (uint08*)pNewTLS + v;

                DbgPrintf("%.01X", (uint32)*bByte);

                if((v+1) % 0x10 == 0 && v+1<stop)
                    DbgPrintf("\nEmuFS (0x%X): 0x%.08X: ", GetCurrentThreadId(), ((uint32)pNewTLS + v));
            }

            DbgPrintf("\n");
        }
        #endif
    }

#ifdef __GNUC__
    __asm__ __volatile__(
        // Obtain "OrgFS"
        "movw %%fs, %%ax\n\t"
        "movw %%ax, %0\n\t"

        // Obtain "OrgNtTib"
        "movl %%fs:0x18, %%eax\n\t"
        "movl %%eax, %1"
        : "=m" (OrgFS),
          "=m" (OrgNtTib)
        :
        : "eax"
    );
#else
    __asm
    {
        // Obtain "OrgFS"
        mov ax, fs
        mov OrgFS, ax

        // Obtain "OrgNtTib"
        mov eax, fs:[0x18]
        mov OrgNtTib, eax
    }
#endif

    // allocate LDT entry
    {
        uint32 dwSize = sizeof(xboxkrnl::KPCR);

        NewPcr = (xboxkrnl::KPCR*)CxbxMalloc(dwSize);

        memset(NewPcr, 0, sizeof(*NewPcr));

        NewFS = EmuAllocateLDT((uint32)NewPcr, (uint32)NewPcr + dwSize);
    }

    // update "OrgFS" with NewFS and (bIsXboxFS = false)
#ifdef __GNUC__
    __asm__ __volatile__(
        "movw %0, %%ax\n\t"
        "movw %%ax, %%fs:0x14\n\t"

        "movb $0, %%al\n\t"
        "movb %%al, %%fs:0x16"
        :
        : "m" (NewFS)
        : "eax"
    );
#else
    __asm
    {
        mov ax, NewFS
        mov bh, 0

        mov fs:[0x14], ax
        mov fs:[0x16], bh
    }
#endif

    // generate TIB
    {
        xboxkrnl::ETHREAD *EThread = (xboxkrnl::ETHREAD*)CxbxMalloc(sizeof(xboxkrnl::ETHREAD));

        EThread->Tcb.TlsData  = (void*)pNewTLS;
        EThread->UniqueThread = GetCurrentThreadId();

        memcpy(&NewPcr->NtTib, OrgNtTib, sizeof(NT_TIB));

        NewPcr->NtTib.Self = &NewPcr->NtTib;

        NewPcr->PrcbData.CurrentThread = (xboxkrnl::KTHREAD*)EThread;

        NewPcr->Prcb = &NewPcr->PrcbData;
    }

    // prepare TLS
    {
        // TLS Index Address := 0
        *(uint32*)pTLS->dwTLSIndexAddr = 0;

        // dword @ pTLSData := pTLSData
        if(pNewTLS != 0)
            *(void**)pNewTLS = pNewTLS;
    }

    // swap into "NewFS"
    EmuSwapFS();

    // update "NewFS" with OrgFS and (bIsXboxFS = true)
#ifdef __GNUC__
    __asm__ __volatile__(
        "movw %0, %%ax\n\t"
        "movw %%ax, %%fs:0x14\n\t"

        "movb $1, %%al\n\t"
        "movb %%al, %%fs:0x16"
        :
        : "m" (OrgFS)
        : "eax"
    );
#else
    __asm
    {
        mov ax, OrgFS
        mov bh, 1

        mov fs:[0x14], ax
        mov fs:[0x16], bh
    }
#endif

    // save "TLSPtr" inside NewFS.StackBase
#ifdef __GNUC__
    __asm__ __volatile__(
        "movl %0, %%eax\n\t"
        "movl %%eax, %%fs:0x04"
        :
        : "m" (pNewTLS)
        : "eax"
    );
#else
    __asm
    {
        mov eax, pNewTLS
        mov fs:[0x04], eax
    }
#endif

    // swap back into the "OrgFS"
    EmuSwapFS();

    DbgPrintf("EmuFS (0x%X): OrgFS=%d NewFS=%d pTLS=0x%.08X\n", GetCurrentThreadId(), OrgFS, NewFS, pTLS);
}

// cleanup fs segment selector emulation
void EmuCleanupFS()
{
    uint16 wSwapFS = 0;

#ifdef __GNUC__
    __asm__ __volatile__(
        "movw %%fs:0x14, %%ax\n\t" // FS.ArbitraryUserPointer
        "movw %%ax, %0"
        : "=m" (wSwapFS)
        :
        : "eax"
    );
#else
    __asm
    {
        mov ax, fs:[0x14]   // FS.ArbitraryUserPointer
        mov wSwapFS, ax
    }
#endif

    if(wSwapFS == 0)
        return;

    if(!EmuIsXboxFS())
        EmuSwapFS();    // Xbox FS

    uint08 *pTLSData = NULL;

#ifdef __GNUC__
    __asm__ __volatile__(
        "movl %%fs:0x04, %%eax\n\t"
        "movl %%eax, %0"
        : "=m" (pTLSData)
        :
        : "eax"
    );
#else
    __asm
    {
        mov eax, fs:[0x04]
        mov pTLSData, eax
    }
#endif

    EmuSwapFS(); // Win2k/XP FS

    if(pTLSData != 0)
        CxbxFree(pTLSData);

    EmuDeallocateLDT(wSwapFS);
}

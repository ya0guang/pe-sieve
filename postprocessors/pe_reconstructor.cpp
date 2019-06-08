#include "pe_reconstructor.h"

#include "../utils/workingset_enum.h"

#include <fstream>

inline bool shift_artefacts(PeArtefacts& artefacts, size_t shift_size)
{
	artefacts.ntFileHdrsOffset += shift_size;
	artefacts.secHdrsOffset += shift_size;
	return true;
}

//WARNING: this function shifts also offsets saved in the artefacts
size_t PeReconstructor::shiftPeHeader()
{
	BYTE *vBuf = this->peBuffer.vBuf;
	const size_t vBufSize = this->peBuffer.vBufSize;
	if (vBuf == nullptr) return 0;

	if (!this->artefacts.hasNtHdrs()) return 0;

	const size_t dos_pe_size = sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_SIGNATURE);
	size_t diff = this->artefacts.ntFileHdrsOffset - this->artefacts.peBaseOffset;
	if (diff >= dos_pe_size) {
		return 0;
	}
	//TODO: shift the header
	if (!this->artefacts.hasSectionHdrs()) return 0; //cannot proceed

	size_t shift_size = dos_pe_size - diff;
	size_t hdrs_end = this->artefacts.secHdrsOffset + (this->artefacts.secCount + 1)* sizeof(IMAGE_SECTION_HEADER);
	if (!peconv::is_padding(vBuf + hdrs_end, shift_size, 0)) {
		return 0; // no empty space, cannot proceed
	}
	size_t hdrs_size = hdrs_end - this->artefacts.peBaseOffset;
	BYTE *new_nt_ptr = vBuf + this->artefacts.peBaseOffset + shift_size;
	if (!peconv::validate_ptr(vBuf, vBufSize, new_nt_ptr, hdrs_size)) {
		return 0;
	}

	size_t pe_offset = (this->artefacts.ntFileHdrsOffset - sizeof(IMAGE_NT_SIGNATURE)) - this->artefacts.peBaseOffset;

	IMAGE_DOS_HEADER dos_template = { 0 };
	dos_template.e_magic = IMAGE_DOS_SIGNATURE;
	dos_template.e_lfanew = pe_offset + shift_size;

	//check mz signature:
	BYTE *mz_ptr = vBuf + this->artefacts.peBaseOffset;
	if (!peconv::validate_ptr(vBuf, vBufSize, mz_ptr, sizeof(IMAGE_DOS_HEADER))) {
		return 0;
	}
	//check PE signature:
	DWORD* pe_ptr = (DWORD*)(vBuf + this->artefacts.peBaseOffset + dos_template.e_lfanew);
	if (!peconv::validate_ptr(vBuf, vBufSize, pe_ptr, sizeof(DWORD))) {
		return false;
	}
	//all checks passed, do the actual headers shift:
	memmove(new_nt_ptr, (vBuf + this->artefacts.peBaseOffset), hdrs_size);

	//write the DOS header:
	memcpy(mz_ptr, &dos_template, sizeof(IMAGE_DOS_HEADER));

	//write the PE signature:
	*pe_ptr = IMAGE_NT_SIGNATURE;

	shift_artefacts(this->artefacts, shift_size);
	return shift_size;
}

bool PeReconstructor::reconstruct(IN HANDLE processHandle)
{
	this->artefacts = origArtefacts;

	ULONGLONG moduleBase = artefacts.regionStart + artefacts.peBaseOffset;
	if (!peBuffer.readRemote(processHandle, moduleBase, artefacts.calculatedImgSize)) {
		return false;
	}
	std::cout << "Reading remote success!\n";

	size_t shift_size = shiftPeHeader();
	if (shift_size) {
		std::cout << "[!] The PE header was shifted by: " << std::hex << shift_size << std::endl;
	}
	bool is_pe_hdr = false;
	if (this->artefacts.hasNtHdrs() && reconstructFileHdr()) {
		is_pe_hdr = reconstructPeHdr();
	}
	if (!is_pe_hdr) {
		return false;
	}
	//do not modify section headers if the PE is in raw format, or no unmapping requested
	if (!peconv::is_pe_raw(peBuffer.vBuf, peBuffer.vBufSize)) {
		if (!fixSectionsVirtualSize(processHandle) || !fixSectionsCharacteristics(processHandle)) {
			return false;
		}
	}
	return true;
}

bool PeReconstructor::fixSectionsVirtualSize(HANDLE processHandle)
{
	BYTE *vBuf = this->peBuffer.vBuf;
	const size_t vBufSize = this->peBuffer.vBufSize;
	if (!vBuf) return false;

	if (!this->artefacts.hasSectionHdrs()) {
		return false;
	}

	ULONGLONG sec_offset = this->artefacts.secHdrsOffset - this->artefacts.peBaseOffset;
	BYTE *hdr_ptr = (sec_offset + vBuf);

	DWORD sec_rva = 0;
	size_t max_sec_size = 0;

	IMAGE_SECTION_HEADER* prev_sec = nullptr;
	IMAGE_SECTION_HEADER* curr_sec = (IMAGE_SECTION_HEADER*)(hdr_ptr);

	const ULONGLONG pe_img_base = (ULONGLONG)artefacts.peImageBase();

	for (size_t i = 0; i < artefacts.secCount; i++, curr_sec++) {
		if (!is_valid_section(vBuf, vBufSize, (BYTE*)curr_sec, IMAGE_SCN_MEM_READ)) {
			break;
		}
		sec_rva = curr_sec->VirtualAddress;
		DWORD sec_size = curr_sec->Misc.VirtualSize;

		ULONGLONG sec_va = pe_img_base + sec_rva;
		size_t real_sec_size = peconv::fetch_region_size(processHandle, (PBYTE)sec_va);
		if (sec_size > real_sec_size) {
			curr_sec->Misc.VirtualSize = DWORD(real_sec_size);
#ifdef _DEBUG
			std::cout << i << "# Fixed section size: " << std::hex
				<< sec_size << " vs real: " << real_sec_size << std::endl;
#endif
		}

		max_sec_size = (real_sec_size > max_sec_size) ? real_sec_size : max_sec_size;

		if (prev_sec && curr_sec->Misc.VirtualSize > 0) {
			ULONGLONG prev_sec_end = prev_sec->VirtualAddress + prev_sec->Misc.VirtualSize;
			if (prev_sec_end > curr_sec->VirtualAddress) {
				if (curr_sec->VirtualAddress > prev_sec->VirtualAddress) {
					DWORD diff = curr_sec->VirtualAddress - prev_sec->VirtualAddress;
					prev_sec->Misc.VirtualSize = diff;
#ifdef _DEBUG
					std::cout << "Trimmed section" << std::endl;
#endif
				}
			}
		}
		if (curr_sec->Misc.VirtualSize > 0) {
			prev_sec = curr_sec;
		}
	}

	if (max_sec_size == 0) {
		return false;
	}
	return true;
}

bool PeReconstructor::fixSectionsCharacteristics(HANDLE processHandle)
{
	BYTE *vBuf = this->peBuffer.vBuf;
	const size_t vBufSize = this->peBuffer.vBufSize;
	if (!vBuf) return false;

	if (!this->artefacts.hasSectionHdrs()) {
		return false;
	}

	ULONGLONG sec_offset = this->artefacts.secHdrsOffset - this->artefacts.peBaseOffset;
	const BYTE *hdr_ptr = (sec_offset + vBuf);
	IMAGE_SECTION_HEADER* curr_sec = (IMAGE_SECTION_HEADER*)(hdr_ptr);

	const DWORD sec_all_flags = IMAGE_SCN_TYPE_NO_PAD
		| IMAGE_SCN_CNT_CODE | IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_CNT_UNINITIALIZED_DATA
		| IMAGE_SCN_LNK_NRELOC_OVFL | IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_MEM_NOT_CACHED
		| IMAGE_SCN_MEM_NOT_PAGED | IMAGE_SCN_MEM_SHARED | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ
		| IMAGE_SCN_MEM_WRITE
		| IMAGE_SCN_NO_DEFER_SPEC_EXC | IMAGE_SCN_GPREL;

	for (size_t i = 0; i < artefacts.secCount; i++, curr_sec++) {
		if (!is_valid_section(vBuf, vBufSize, (BYTE*)curr_sec, 0)) {
			break;
		}
		//leave only the flags that are valid
		const DWORD charact = curr_sec->Characteristics;
		curr_sec->Characteristics = charact & sec_all_flags;
#ifdef DEBUG
		if (charact != curr_sec->Characteristics) {
			std::cout << "Section characteristics overwriten\n";
		}
#endif
	}
	return true;
}

bool PeReconstructor::reconstructFileHdr()
{
	BYTE *vBuf = this->peBuffer.vBuf;
	const size_t vBufSize = this->peBuffer.vBufSize;
	if (!vBuf) return false;

	if (!this->artefacts.hasNtHdrs()) {
		return false;
	}
	size_t nt_offset = this->artefacts.ntFileHdrsOffset - this->artefacts.peBaseOffset;
	BYTE* nt_ptr = (BYTE*)((ULONGLONG)vBuf + nt_offset);
	if (is_valid_file_hdr(vBuf, vBufSize, nt_ptr, 0)) {
		return true;
	}
	IMAGE_FILE_HEADER* hdr_candidate = (IMAGE_FILE_HEADER*)nt_ptr;
	if (!peconv::validate_ptr(vBuf, vBufSize, hdr_candidate, sizeof(IMAGE_FILE_HEADER))) {
		// probably buffer finished
		return false;
	}

	size_t opt_hdr_size = 0;
	if (artefacts.is64bit) {
		hdr_candidate->Machine = IMAGE_FILE_MACHINE_AMD64;
		opt_hdr_size = sizeof(IMAGE_OPTIONAL_HEADER64);
	}
	else {
		hdr_candidate->Machine = IMAGE_FILE_MACHINE_I386;
		opt_hdr_size = sizeof(IMAGE_OPTIONAL_HEADER32);
	}
	if (this->artefacts.secHdrsOffset) {
		size_t calc_offset = this->artefacts.secHdrsOffset - (nt_offset + sizeof(IMAGE_FILE_HEADER));
		if (calc_offset != opt_hdr_size) {
			std::cout << "[WARNING] Calculated sections header offset is different than the saved one!\n";
		}
		hdr_candidate->NumberOfSections = WORD(this->artefacts.secCount);
		hdr_candidate->SizeOfOptionalHeader = WORD(calc_offset);
	}

	hdr_candidate->NumberOfSymbols = 0;
	hdr_candidate->PointerToSymbolTable = 0;
	return true;
}

bool PeReconstructor::reconstructPeHdr()
{
	BYTE *vBuf = this->peBuffer.vBuf;
	const size_t vBufSize = this->peBuffer.vBufSize;
	if (!vBuf) return false;

	if (!this->artefacts.hasNtHdrs()) {
		return false;
	}
	ULONGLONG nt_offset = this->artefacts.ntFileHdrsOffset - this->artefacts.peBaseOffset;
	BYTE* nt_ptr = (BYTE*)((ULONGLONG)vBuf + nt_offset);
	BYTE *pe_ptr = nt_ptr - sizeof(DWORD);

	if (!peconv::validate_ptr(vBuf, vBufSize, pe_ptr, sizeof(DWORD))) {
		return false;
	}
	IMAGE_NT_HEADERS32 *nt32 = (IMAGE_NT_HEADERS32*)pe_ptr;
	//write signature:
	nt32->Signature = IMAGE_NT_SIGNATURE;
	IMAGE_FILE_HEADER *file_hdr = &nt32->FileHeader;

	bool is64bit = (file_hdr->Machine == IMAGE_FILE_MACHINE_AMD64) ? true : false;

	if (nt32->FileHeader.SizeOfOptionalHeader == 0) {
		nt32->FileHeader.SizeOfOptionalHeader = is64bit ? sizeof(IMAGE_OPTIONAL_HEADER64) : sizeof(IMAGE_OPTIONAL_HEADER32);
	}
	LONG pe_offset = LONG((ULONGLONG)pe_ptr - (ULONGLONG)vBuf);
	IMAGE_DOS_HEADER* dosHdr = (IMAGE_DOS_HEADER*) vBuf;
	dosHdr->e_magic = IMAGE_DOS_SIGNATURE;
	dosHdr->e_lfanew = pe_offset;

	bool is_fixed = false;
	if (is64bit) {
		is_fixed = overwrite_opt_hdr<IMAGE_OPTIONAL_HEADER64>(vBuf, vBufSize, (IMAGE_OPTIONAL_HEADER64*)&nt32->OptionalHeader, this->artefacts);
	}
	else {
		is_fixed = overwrite_opt_hdr<IMAGE_OPTIONAL_HEADER32>(vBuf, vBufSize, &nt32->OptionalHeader, this->artefacts);
	}
	if (!is_fixed) {
		return false;
	}
	if (!peconv::get_nt_hrds(vBuf)) {
		return false;
	}
	return true;
}


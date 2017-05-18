#define KERNEL_PRIVATE 1
#include "mach/vm_statistics.h"
#undef KERNEL_PRIVATE

#include "memctl/kernel_memory.h"

#include "memctl/core.h"
#include "memctl/kernel.h"
#include "memctl/kernel_call.h"
#include "memctl/memory_region.h"
#include "memctl/utility.h"

#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <unistd.h>

/*
 * transfer_fn
 *
 * Description:
 * 	The type of a function to transfer memory between user space and the kernel.
 */
typedef kernel_io_result (*transfer_fn)(
		kaddr_t kaddr,
		size_t size,
		void *data,
		size_t access,
		bool into_kernel);

/*
 * transfer_range_fn
 *
 * Description:
 * 	The type of a function to find the range of memory that can be transferred between user
 * 	space and the kernel.
 */
typedef kernel_io_result (*transfer_range_fn)(
		kaddr_t kaddr,
		size_t *size,
		size_t *width,
		kaddr_t *next,
		bool into_kernel);

// The read/write functions.
kernel_read_fn  kernel_read_unsafe;
kernel_write_fn kernel_write_unsafe;
kernel_read_fn  kernel_read_heap;
kernel_write_fn kernel_write_heap;
kernel_read_fn  kernel_read_safe;
kernel_write_fn kernel_write_safe;
kernel_read_fn  kernel_read_all;
kernel_write_fn kernel_write_all;
kernel_read_fn  physical_read_unsafe;
kernel_write_fn physical_write_unsafe;

// pmap_t kernel_pmap;
static kaddr_t kernel_pmap;

// ppnum_t pmap_find_phys(pmap_t map, addr64_t va);
static kaddr_t _pmap_find_phys;

// UInt<N> IOMappedRead<N>(IOPhysicalAddress address)
static kaddr_t _IOMappedRead8;
static kaddr_t _IOMappedRead16;
static kaddr_t _IOMappedRead32;
static kaddr_t _IOMappedRead64;

// void IOMappedWrite<N>(IOPhysicalAddress address, UInt<N> value)
static kaddr_t _IOMappedWrite8;
static kaddr_t _IOMappedWrite16;
static kaddr_t _IOMappedWrite32;
static kaddr_t _IOMappedWrite64;

/*
 * mach_unexpected
 *
 * Description:
 * 	Generate an internal error due to the given mach call returning an unexpected error code.
 */
static void
mach_unexpected(const char *function, kern_return_t kr) {
	error_internal("%s returned %d: %s", function, kr, mach_error_string(kr));
}

/*
 * ilog2
 *
 * Description:
 * 	Integer logarithm base 2. ilog2(0) is 0.
 */
static unsigned
ilog2(uint64_t n) {
	unsigned log = 0;
	for (;;) {
		n >>= 1;
		if (n == 0) {
			return log;
		}
		log += 1;
	}
}

/*
 * transfer_unsafe
 *
 * Description:
 * 	A transfer function that performs direct virtual memory reads and writes. This is generally
 * 	unsafe.
 */
static kernel_io_result
transfer_unsafe(kaddr_t kaddr, size_t size, void *data, size_t access, bool into_kernel) {
	assert(access != 0);
	uint8_t *p = (uint8_t *)data;
	while (size > 0) {
		size_t copysize = min(size, page_size - (kaddr & page_mask));
		if (access < sizeof(kword_t) && access < copysize) {
			copysize = access;
		}
		kern_return_t kr;
		if (into_kernel) {
			kr = mach_vm_write(kernel_task, kaddr, (vm_offset_t)p, copysize);
		} else {
			mach_vm_size_t out_size = copysize;
			kr = mach_vm_read_overwrite(kernel_task, kaddr, copysize,
					(mach_vm_address_t)p, &out_size);
		}
		if (kr != KERN_SUCCESS) {
			if (kr == KERN_PROTECTION_FAILURE) {
				return KERNEL_IO_PROTECTION;
			} else {
				const char *fn = (into_kernel ? "mach_vm_write"
				                              : "mach_vm_read_overwrite");
				mach_unexpected(fn, kr);
				return KERNEL_IO_ERROR;
			}
		}
		kaddr += copysize;
		p     += copysize;
		size  -= copysize;
	}
	return KERNEL_IO_SUCCESS;
}

/*
 * physical_word_read_unsafe
 *
 * Description:
 * 	Read a word of physical memory.
 */
static bool
physical_word_read_unsafe(paddr_t paddr, void *data, size_t logsize) {
	assert(logsize <= 3);
	const kaddr_t fn[4] = {
		_IOMappedRead8, _IOMappedRead16, _IOMappedRead32, _IOMappedRead64
	};
	bool success = kernel_call(data, 1 << logsize, fn[logsize], 1, &paddr);
	if (!success) {
		error_internal("could not read physical address 0x%llx", (long long)paddr);
	}
	return success;
}

/*
 * physical_word_write_unsafe
 *
 * Description:
 * 	Write a word of physical memory.
 */
static bool
physical_word_write_unsafe(paddr_t paddr, uint64_t data, size_t logsize) {
	assert(logsize <= 3);
	const kaddr_t fn[4] = {
		_IOMappedWrite8, _IOMappedWrite16, _IOMappedWrite32, _IOMappedWrite64
	};
	kword_t args[2] = { paddr, data };
	bool success = kernel_call(NULL, 0, fn[logsize], 2, args);
	if (!success) {
		error_internal("could not write physical address 0x%llx", (long long)paddr);
	}
	return success;
}

/*
 * transfer_physical_words_unsafe
 *
 * Description:
 * 	A transfer function that performs direct physical memory reads and writes using word-sized
 * 	transfers. This is generally unsafe.
 *
 * Notes:
 * 	Since at most 8 bytes can be transferred per kernel call, this operation is slow.
 */
static kernel_io_result
transfer_physical_words_unsafe(paddr_t paddr, size_t size, void *data, size_t access,
		bool into_kernel) {
	assert(access != 0);
	uint8_t *p = (uint8_t *)data;
	kword_t dummy_args[] = { 1 };
	bool trunc_32 = !into_kernel && !kernel_call(NULL, sizeof(uint64_t), 0, 1, dummy_args);
	while (size > 0) {
		size_t wordsize = min(size, sizeof(kword_t) - (paddr & page_mask));
		if (access < wordsize) {
			wordsize = access;
		}
		if (wordsize == sizeof(uint64_t) && trunc_32) {
			wordsize = sizeof(uint32_t);
		}
		size_t logsize = ilog2(wordsize);
		wordsize = 1 << logsize;
		bool success;
		if (into_kernel) {
			success = physical_word_write_unsafe(paddr, unpack_uint(p, wordsize),
					logsize);
		} else {
			success = physical_word_read_unsafe(paddr, p, logsize);
		}
		if (!success) {
			return KERNEL_IO_ERROR;
		}
		paddr += wordsize;
		p     += wordsize;
		size  -= wordsize;
	}
	return KERNEL_IO_SUCCESS;
}

/*
 * transfer_range_unsafe
 *
 * Description:
 * 	A transfer range function that assumes the whole virtual or physical range to be
 * 	transferred is valid. This is generally unsafe.
 */
static kernel_io_result
transfer_range_unsafe(kaddr_t addr, size_t *size, size_t *access, kaddr_t *next, bool into_kernel) {
	if (next != NULL) {
		*next = addr + *size;
	}
	if (*access == 0) {
		*access = sizeof(kword_t);
	}
	return KERNEL_IO_SUCCESS;
}

/*
 * region_is_heap
 *
 * Description:
 * 	Returns whether the region is on the heap.
 */
static bool
region_is_heap(vm_region_submap_short_info_64_t info) {
	int prot = VM_PROT_READ | VM_PROT_WRITE;
	return ((info->protection & prot) == prot
	        && info->share_mode != SM_EMPTY
	        && (info->user_tag == VM_KERN_MEMORY_ZONE
	            || info->user_tag == VM_KERN_MEMORY_KALLOC));
}

/*
 * transfer_range_heap
 *
 * Description:
 * 	Find the transfer range for the given transfer, but only consider heap regions.
 */
static kernel_io_result
transfer_range_heap(kaddr_t kaddr, size_t *size, size_t *access, kaddr_t *next, bool into_kernel) {
	size_t left = *size;
	size_t transfer_size = 0;
	kaddr_t next_viable = kaddr + left;
	kernel_io_result result = KERNEL_IO_SUCCESS;
	while (left > 0) {
		mach_vm_address_t address = kaddr;
		mach_vm_size_t vmsize = 0;
		uint32_t depth = 2048;
		vm_region_submap_short_info_data_64_t info;
		mach_msg_type_number_t count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
		kern_return_t kr = mach_vm_region_recurse(kernel_task, &address, &vmsize,
				&depth, (vm_region_recurse_info_t)&info, &count);
		if (kr != KERN_SUCCESS) {
			if (kr == KERN_INVALID_ADDRESS) {
				// We've reached the end of the kernel memory map.
				next_viable = 0;
				result = KERNEL_IO_UNMAPPED;
				break;
			}
			next_viable = (kaddr & ~page_mask) + page_size;
			mach_unexpected("mach_vm_region_recurse", kr);
			result = KERNEL_IO_ERROR;
			break;
		}
		bool viable = region_is_heap(&info);
		if (address > kaddr) {
			result = KERNEL_IO_UNMAPPED;
			if (viable) {
				next_viable = address;
			} else {
				next_viable = address + vmsize;
			}
			break;
		}
		if (!viable) {
			result = KERNEL_IO_PROTECTION;
			next_viable = address + vmsize;
			break;
		}
		// Incorporate the region.
		size_t region_size = min(left, vmsize - (kaddr - address));
		kaddr += region_size;
		left -= region_size;
		transfer_size += region_size;
	}
	*size = transfer_size;
	if (next != NULL) {
		*next = next_viable;
	}
	if (*access == 0) {
		*access = sizeof(kword_t);
	}
	return result;
}

/*
 * region_looks_safe
 *
 * Description:
 * 	Returns whether the region looks safe for the given operation.
 */
static bool
region_looks_safe(vm_region_submap_short_info_64_t info, bool into_kernel) {
	int prot = (into_kernel ? VM_PROT_WRITE : VM_PROT_READ);
	return ((info->protection & prot) == prot
	        && info->share_mode != SM_EMPTY);
}

/*
 * transfer_range_safe
 *
 * Description:
 * 	Find the transfer range for the given transfer, but only consider safe regions.
 */
static kernel_io_result
transfer_range_safe(kaddr_t kaddr, size_t *size, size_t *access, kaddr_t *next, bool into_kernel) {
	size_t left = *size;
	size_t transfer_size = 0;
	kaddr_t next_viable = kaddr + left;
	kernel_io_result result = KERNEL_IO_SUCCESS;
	while (result == KERNEL_IO_SUCCESS && left > 0) {
		// First, check if the virtual memory region looks viable. If not, then abort the
		// loop, since no data can be transferred from address kaddr.
		mach_vm_address_t address = kaddr;
		mach_vm_size_t vmsize = 0;
		uint32_t depth = 2048;
		vm_region_submap_short_info_data_64_t info;
		mach_msg_type_number_t count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
		kern_return_t kr = mach_vm_region_recurse(kernel_task, &address, &vmsize,
				&depth, (vm_region_recurse_info_t)&info, &count);
		if (kr != KERN_SUCCESS) {
			if (kr == KERN_INVALID_ADDRESS) {
				// We've reached the end of the kernel memory map.
				next_viable = 0;
				result = KERNEL_IO_UNMAPPED;
				break;
			}
			next_viable = (kaddr & ~page_mask) + page_size;
			mach_unexpected("mach_vm_region_recurse", kr);
			result = KERNEL_IO_ERROR;
			break;
		}
		bool viable = region_looks_safe(&info, into_kernel);
		if (address > kaddr) {
			result = KERNEL_IO_UNMAPPED;
			if (viable) {
				next_viable = address;
			} else {
				next_viable = address + vmsize;
			}
			break;
		}
		if (!viable) {
			result = KERNEL_IO_PROTECTION;
			next_viable = address + vmsize;
			break;
		}
		// Next, check to see how many pages starting at kaddr are actually mapped. Here we
		// do complete the rest of the loop, since some data may be transferrable.
		size_t region_size = min(left, vmsize - (kaddr - address));
		error_stop();
		for (kaddr_t unmapped = kaddr & ~page_mask; unmapped < kaddr + region_size;
				unmapped += page_size) {
			paddr_t paddr = 0;
			kernel_virtual_to_physical(unmapped, &paddr);
			if (paddr == 0) {
				// We've encountered an unmapped page before the end of the current
				// region. Truncate our region to this smaller size.
				result = KERNEL_IO_UNMAPPED;
				region_size = (unmapped > kaddr ? unmapped - kaddr : 0);
				next_viable = unmapped + page_size;
				break;
			}
		}
		error_start();
		// Incorporate the region.
		kaddr += region_size;
		left -= region_size;
		transfer_size += region_size;
	}
	*size = transfer_size;
	if (next != NULL) {
		*next = next_viable;
	}
	if (*access == 0) {
		*access = sizeof(kword_t);
	}
	return result;
}

/*
 * transfer_range_all
 *
 * Description:
 * 	Find the transfer range for the given transfer, checking virtual and physical addresses to
 * 	see if the memory is mapped and safe to access.
 */
static kernel_io_result
transfer_range_all(kaddr_t kaddr, size_t *size, size_t *access, kaddr_t *next, bool into_kernel) {
	bool default_access = (*access == 0);
	size_t remaining = *size;
	size_t transfer_size = 0;
	size_t transfer_access = 0;
	kaddr_t next_viable = kaddr + remaining;
	kernel_io_result result = KERNEL_IO_SUCCESS;
	error_stop();
	while (remaining > 0) {
		size_t this_access = 0;
		size_t this_size   = remaining;
		// Check the virtual address.
		const struct memory_region *vr = virtual_region_find(kaddr, this_size);
		if (vr == NULL) {
			// Everything's good; do nothing.
		} else if (kaddr < vr->start) {
			this_size = vr->start - kaddr;
		} else if (default_access && vr->access == 0) {
			result = KERNEL_IO_INACCESSIBLE;
			next_viable = vr->end + 1;
			break;
		} else {
			this_access = vr->access;
			this_size   = min(this_size, vr->end + 1 - kaddr);
		}
		// Check the physical address.
		paddr_t paddr = 0;
		kernel_virtual_to_physical(kaddr, &paddr);
		if (paddr == 0) {
			result = KERNEL_IO_UNMAPPED;
			next_viable = (kaddr & ~page_mask) + page_size;
			break;
		}
		size_t phys_size = min(this_size, page_size - (kaddr & page_mask));
		const struct memory_region *pr = physical_region_find(paddr, phys_size);
		if (pr == NULL) {
			this_size = phys_size;
		} else if (paddr < pr->start) {
			this_size = pr->start - paddr;
		} else if (default_access && pr->access == 0) {
			result = KERNEL_IO_INACCESSIBLE;
			next_viable = (kaddr & ~page_mask) + page_size;
			break;
		} else {
			if (this_access != 0 && this_access != pr->access) {
				// We need two contradictory access widths?
				result = KERNEL_IO_INACCESSIBLE;
				next_viable = (kaddr & ~page_mask) + page_size;
				break;
			}
			this_access = pr->access;
			this_size   = min(phys_size, pr->end + 1 - paddr);
		}
		// If there's been no restrictions on the access width so far, default to the
		// kernel word size.
		if (this_access == 0) {
			this_access = sizeof(kword_t);
		}
		if (this_access != transfer_access) {
			if (transfer_access == 0) {
				transfer_access = this_access;
			} else {
				next_viable = kaddr;
				break;
			}
		}
		kaddr += this_size;
		remaining -= this_size;
		transfer_size += this_size;
	}
	error_start();
	*size = transfer_size;
	if (next != NULL) {
		*next = next_viable;
	}
	if (default_access) {
		*access = transfer_access;
	}
	return result;
}

/*
 * kernel_io
 *
 * Description:
 * 	Run the transfer between user space and the kernel with the given transfer functions.
 */
static kernel_io_result
kernel_io(kaddr_t kaddr, size_t *size, void *data, size_t access, kaddr_t *next,
		transfer_range_fn transfer_range, transfer_fn transfer, bool write) {
	kernel_io_result result = KERNEL_IO_SUCCESS;
	size_t left = *size;
	kaddr_t start = kaddr;
	uint8_t *p = (uint8_t *)data;
	while (left > 0) {
		size_t transfer_size = left;
		size_t transfer_access = access;
		result = transfer_range(kaddr, &transfer_size, &transfer_access, next, write);
		if (result != KERNEL_IO_SUCCESS) {
			left = transfer_size;
		}
		kernel_io_result result2 = transfer(kaddr, transfer_size, p, transfer_access,
				write);
		if (result2 != KERNEL_IO_SUCCESS) {
			result = result2;
			break;
		}
		kaddr += transfer_size;
		left -= transfer_size;
		p += transfer_size;
	}
	switch (result) {
		case KERNEL_IO_SUCCESS:                                         break;
		case KERNEL_IO_ERROR:        error_kernel_io(kaddr);            break;
		case KERNEL_IO_PROTECTION:   error_address_protection(kaddr);   break;
		case KERNEL_IO_UNMAPPED:     error_address_unmapped(kaddr);     break;
		case KERNEL_IO_INACCESSIBLE: error_address_inaccessible(kaddr); break;
	}
	*size = kaddr - start;
	return result;
}

kernel_io_result
kernel_read_word(kernel_read_fn read, kaddr_t kaddr, void *value, size_t width,
		size_t access_width) {
	return read(kaddr, &width, value, access_width, NULL);
}

kernel_io_result
kernel_write_word(kernel_write_fn write, kaddr_t kaddr, kword_t value, size_t width,
		size_t access_width) {
	pack_uint(&value, value, width);
	return write(kaddr, &width, &value, access_width, NULL);
}

static kernel_io_result
kernel_read_unsafe_(kaddr_t kaddr, size_t *size, void *data, size_t access_width, kaddr_t *next) {
	return kernel_io(kaddr, size, data, access_width, next, transfer_range_unsafe,
			transfer_unsafe, false);
}

static kernel_io_result
kernel_write_unsafe_(kaddr_t kaddr, size_t *size, const void *data, size_t access_width,
		kaddr_t *next) {
	return kernel_io(kaddr, size, (void *)data, access_width, next, transfer_range_unsafe,
			transfer_unsafe, true);
}

static kernel_io_result
kernel_read_heap_(kaddr_t kaddr, size_t *size, void *data, size_t access_width, kaddr_t *next) {
	return kernel_io(kaddr, size, data, access_width, next, transfer_range_heap,
			transfer_unsafe, false);
}

static kernel_io_result
kernel_write_heap_(kaddr_t kaddr, size_t *size, const void *data, size_t access_width,
		kaddr_t *next) {
	return kernel_io(kaddr, size, (void *)data, access_width, next, transfer_range_heap,
			transfer_unsafe, true);
}

static kernel_io_result
kernel_read_safe_(kaddr_t kaddr, size_t *size, void *data, size_t access_width, kaddr_t *next) {
	return kernel_io(kaddr, size, data, access_width, next, transfer_range_safe,
			transfer_unsafe, false);
}

static kernel_io_result
kernel_write_safe_(kaddr_t kaddr, size_t *size, const void *data, size_t access_width,
		kaddr_t *next) {
	return kernel_io(kaddr, size, (void *)data, access_width, next, transfer_range_safe,
			transfer_unsafe, true);
}

static kernel_io_result
kernel_read_all_(kaddr_t kaddr, size_t *size, void *data, size_t access_width, kaddr_t *next) {
	return kernel_io(kaddr, size, data, access_width, next, transfer_range_all,
			transfer_unsafe, false);
}

static kernel_io_result
kernel_write_all_(kaddr_t kaddr, size_t *size, const void *data, size_t access_width,
		kaddr_t *next) {
	return kernel_io(kaddr, size, (void *)data, access_width, next, transfer_range_all,
			transfer_unsafe, true);
}

static kernel_io_result
physical_read_unsafe_(paddr_t paddr, size_t *size, void *data, size_t access_width,
		kaddr_t *next) {
	return kernel_io(paddr, size, data, access_width, next, transfer_range_unsafe,
			transfer_physical_words_unsafe, false);
}

static kernel_io_result
physical_write_unsafe_(paddr_t paddr, size_t *size, const void *data, size_t access_width,
		kaddr_t *next) {
	return kernel_io(paddr, size, (void *)data, access_width, next, transfer_range_unsafe,
			transfer_physical_words_unsafe, true);
}

bool
kernel_virtual_to_physical(kaddr_t kaddr, paddr_t *paddr) {
	ppnum_t ppnum;
	kword_t args[] = { kernel_pmap, kaddr };
	bool success = kernel_call(&ppnum, sizeof(ppnum), _pmap_find_phys, 2, args);
	if (!success) {
		error_internal("could not call %s", "_pmap_find_phys");
		return false;
	}
	if (ppnum == 0) {
		*paddr = 0;
	} else {
		*paddr = ((paddr_t)ppnum << page_shift) | (kaddr & page_mask);
	}
	return true;
}

bool
kernel_memory_init() {
#define SET(fn)									\
	if (fn == NULL) {							\
		fn = fn##_;							\
	}
#define DO_RESOLVE(sym)								\
	kext_result kr = kernel_symbol(#sym, &sym, NULL);			\
	if (kr != KEXT_SUCCESS) {						\
		error_internal("could not resolve %s", #sym);			\
		return false;							\
	}
#define RESOLVE(sym)								\
	if (sym == 0) {								\
		DO_RESOLVE(sym);						\
	}
#define READ(var)								\
	if (var == 0) {								\
		kaddr_t _##var;							\
		DO_RESOLVE(_##var);						\
		kernel_io_result kior = kernel_read_word(kernel_read_unsafe,	\
				_##var, &var, sizeof(var), 0);			\
		if (kior != KERNEL_IO_SUCCESS) {				\
			error_internal("could not read %s", "_"#var);		\
			return false;						\
		}								\
	}

	// Load the basic functionality provided by kernel_task.
	if (kernel_task == MACH_PORT_NULL) {
		return true;
	}
	SET(kernel_read_unsafe);
	SET(kernel_write_unsafe);
	SET(kernel_read_heap);
	SET(kernel_write_heap);
	// Load symbols and kernel variable values.
	if (kernel.base == 0 || kernel.slide == 0) {
		return true;
	}
	READ(kernel_pmap);
	RESOLVE(_pmap_find_phys);
	RESOLVE(_IOMappedRead8);
	RESOLVE(_IOMappedRead16);
	RESOLVE(_IOMappedRead32);
	RESOLVE(_IOMappedRead64);
	RESOLVE(_IOMappedWrite8);
	RESOLVE(_IOMappedWrite16);
	RESOLVE(_IOMappedWrite32);
	RESOLVE(_IOMappedWrite64);
	// Load the functions that depend on kernel_virtual_to_physical.
	kword_t kv2p_args[2] = { kernel_pmap, kernel.base };
	if (kernel_call(NULL, sizeof(ppnum_t), 0, 2, kv2p_args)) {
		SET(kernel_read_safe);
		SET(kernel_write_safe);
		SET(kernel_read_all);
		SET(kernel_write_all);
	}
	kword_t dummy_args[2] = { 1, 1 };
	if (kernel_call(NULL, sizeof(uint32_t), 0, 1, dummy_args)) {
		SET(physical_read_unsafe);
	}
	if (kernel_call(NULL, 0, 0, 2, dummy_args)) {
		SET(physical_write_unsafe);
	}
	return true;
#undef SET
#undef DO_RESOLVE
#undef RESOLVE
#undef READ
}
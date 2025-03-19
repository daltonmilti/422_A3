/**
 * helloModule.c - Linux Kernel Module for Reporting Allocated Pages
 *
 * This module iterates over all running processes with a PID greater than 650,
 * examines their virtual memory areas, and counts the number of physical pages
 * allocated. It further differentiates between pages that are mapped contiguously
 * versus non-contiguously in physical memory.
 *
 * The results are printed in CSV format to the kernel log.
 *
 * NOTE: This module is best used on Linux kernel version 5.x. If you are running a
 * kernel 6.x system, unexpected behavior may occur due to changes in memory management.
 */

//----------------------------------
//           HEADER FILES
//----------------------------------
#include <linux/module.h>       // Core kernel module definitions
#include <linux/kernel.h>       // For printk logging macros
#include <linux/init.h>         // For __init and __exit macros
#include <linux/sched.h>        // For task_struct and process iteration
#include <linux/sched/signal.h> // For for_each_process() macro
#include <linux/mm.h>           // For mm_struct and vm_area_struct
#include <linux/mm_types.h>     // For memory structure types
#include <linux/pid_namespace.h>// For PID namespace support
#include <asm/io.h>             // For PAGE_MASK and I/O operations
#include <asm/pgtable.h>        // For page table related functions/macros
#include <linux/highmem.h>      // For pte_offset_map() and pte_unmap()

MODULE_AUTHOR("Dalton Mlitimore");     // Author name
MODULE_DESCRIPTION("Kernel module that reports allocated physical pages per process");
MODULE_VERSION("0.1");          // Version of the module

/**
 * virt2phys - Convert a virtual page address to its corresponding physical address.
 * @mm:   Pointer to the memory map of the process.
 * @vaddr: Virtual address to be translated.
 *
 * Returns the physical address if the page is present; otherwise returns 0.
 */
unsigned long virt2phys(struct mm_struct *mm, unsigned long vaddr)
{
    pgd_t *pgd_entry;           // Page Global Directory pointer
    p4d_t *p4d_entry;           // Page 4th-level Directory pointer
    pud_t *pud_entry;           // Page Upper Directory pointer
    pmd_t *pmd_entry;           // Page Middle Directory pointer
    pte_t *pte_entry;           // Page Table Entry pointer
    struct page *page_ptr;      // Pointer to the "struct page" for this address

    // Retrieve the first-level (PGD) entry.
    pgd_entry = pgd_offset(mm, vaddr);  
    if (pgd_none(*pgd_entry) || pgd_bad(*pgd_entry))
        return 0;               // If PGD doesn't exist or is invalid, return 0.

    // Retrieve the 4th-level (P4D) entry.
    p4d_entry = p4d_offset(pgd_entry, vaddr); 
    if (p4d_none(*p4d_entry) || p4d_bad(*p4d_entry))
        return 0;               // If P4D is invalid, return 0.

    // Retrieve the upper-level (PUD) entry.
    pud_entry = pud_offset(p4d_entry, vaddr);
    if (pud_none(*pud_entry) || pud_bad(*pud_entry))
        return 0;               // If PUD is invalid, return 0.

    // Retrieve the middle-level (PMD) entry.
    pmd_entry = pmd_offset(pud_entry, vaddr);
    if (pmd_none(*pmd_entry) || pmd_bad(*pmd_entry))
        return 0;               // If PMD is invalid, return 0.

    // Map the page table entry (PTE) into kernel space.
    pte_entry = pte_offset_map(pmd_entry, vaddr);
    if (!pte_entry)
        return 0;               // If pte_offset_map failed, return 0.

    // Check if the PTE corresponds to a valid physical page.
    page_ptr = pte_page(*pte_entry);
    if (!page_ptr) {
        pte_unmap(pte_entry);   // Unmap before returning
        return 0;
    }

    // Obtain the physical address from the page structure.
    {
        unsigned long phys_addr = page_to_phys(page_ptr); 
        pte_unmap(pte_entry);   // Unmap the PTE now that we're done

        // Optional check if this address is a special "unmapped" sentinel.
        if (phys_addr == 70368744173568ULL)
            return 0;

        return phys_addr;       // Return the resolved physical address
    }
}

/**
 * count_allocated_pages - Count physical pages for a given process.
 * @task:       Pointer to the process's task_struct.
 * @total:      Pointer to the total allocated pages count.
 * @contig:     Pointer to count of contiguous pages (optional).
 * @noncontig:  Pointer to count of non-contiguous pages (optional).
 *
 * This function iterates over each virtual memory area (VMA) in the process's
 * memory map and, for every page, it attempts to translate the virtual address
 * to a physical address. If successful, it updates the page counts.
 */
void count_allocated_pages(struct task_struct *task, unsigned long *total,
                           unsigned long *contig, unsigned long *noncontig)
{
    struct vm_area_struct *area;  // Used to walk the list of VM areas
    unsigned long vaddr;          // Current virtual address in the VMA
    unsigned long prev_phys = 0;  // Stores previous page's physical address

    // Initialize counts to zero before we start.
    *total = 0;
    if (contig)
        *contig = 0;
    if (noncontig)
        *noncontig = 0;

    // Check if the task's mm (memory map) exists and has a starting VMA.
    if (task->mm && task->mm->mmap) {
        // For every virtual memory area in this task
        for (area = task->mm->mmap; area; area = area->vm_next) {
            // Step through each page in the VMA.
            for (vaddr = area->vm_start; vaddr < area->vm_end; vaddr += PAGE_SIZE) {
                // Translate virtual address to physical address.
                unsigned long phys = virt2phys(task->mm, vaddr);

                if (phys == 0)
                    continue;          // Skip if it's not mapped (unallocated)

                // We found a valid physical page, increment total count.
                (*total)++;

                // If contig and noncontig pointers are valid, check contiguity.
                if (contig && noncontig) {
                    // If this is not the first page in the VMA
                    if (prev_phys != 0) {
                        // Check if current physical address is PAGE_SIZE after the previous
                        if (phys == prev_phys + PAGE_SIZE)
                            (*contig)++;
                        else
                            (*noncontig)++;
                    }
                    // Update prev_phys to current page's address
                    prev_phys = phys;
                }
            }
        }
    }

    // Adjust for the very first valid page encountered in the entire region.
    // If we counted at least 1 total page, but contig+noncontig is still one less,
    // mark the first page as non-contiguous by default.
    if (contig && noncontig && *total > 0 && ((*contig + *noncontig) < *total))
        (*noncontig)++;
}

/**
 * generate_report - Print the process report in CSV format.
 *
 * Iterates over all processes with a PID > 650, computes the page counts, and logs
 * a CSV-formatted report to the kernel log.
 */
static void generate_report(void)
{
    struct task_struct *proc;       // For iterating through processes
    unsigned long proc_total;       // Holds total pages for a single process
    unsigned long proc_contig;      // Holds contiguous count for a single process
    unsigned long proc_noncontig;   // Holds non-contiguous count for a single process
    unsigned long grand_total = 0;  // Accumulates total pages for all processes
    unsigned long grand_contig = 0; // Accumulates contiguous pages for all processes
    unsigned long grand_noncontig = 0; // Accumulates non-contiguous pages

    printk(KERN_INFO "PROCESS REPORT:\n"); 
    // Print CSV header: pid, name, contig, noncontig, total
    printk(KERN_INFO "proc_id,proc_name,contig_pages,noncontig_pages,total_pages\n");

    // for_each_process() macro iterates through every task_struct in the system.
    for_each_process(proc) {
        // We only care about processes with PID > 650
        if (proc->pid > 650) {
            // Count allocated pages for this process
            count_allocated_pages(proc, &proc_total, &proc_contig, &proc_noncontig);

            // Print the CSV line for this process
            printk(KERN_INFO "%d,%s,%lu,%lu,%lu\n",
                   proc->pid, proc->comm, proc_contig, proc_noncontig, proc_total);

            // Accumulate grand totals
            grand_total     += proc_total;
            grand_contig    += proc_contig;
            grand_noncontig += proc_noncontig;
        }
    }

    // Print total line in CSV format
    printk(KERN_INFO "TOTALS,,%lu,%lu,%lu\n",
           grand_contig, grand_noncontig, grand_total);
}

/**
 * helloModule_init - Module initialization routine.
 *
 * Logs the start of the module, generates the process report,
 * and confirms successful loading.
 */
static int __init helloModule_init(void)
{
    printk(KERN_INFO "helloModule: Initializing module...\n");
    generate_report(); // Generate the CSV-style process report
    printk(KERN_INFO "helloModule: Module loaded successfully.\n");
    return 0;          // Return 0 to indicate successful init
}

/**
 * helloModule_exit - Module cleanup routine.
 *
 * Logs the module unloading.
 */
static void __exit helloModule_exit(void)
{
    printk(KERN_INFO "helloModule: Module unloaded.\n");
}

// Specify the functions to call on module load/unload
module_init(helloModule_init);
module_exit(helloModule_exit);

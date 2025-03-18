/*
 * procReport.c - Linux Kernel Module for Page Table Walking and Process Report
 *
 * This module traverses all processes in the system and, for each process with a PID > 650,
 * it examines the process's virtual memory areas (VMAs) to count the total number of pages
 * allocated in physical memory. In addition, it optionally counts how many pages are mapped
 * contiguously versus non-contiguously.
 *
 * The report is printed to the kernel log in CSV format.
 *
 * NOTE: This module is intended for use on kernel version 5.x. If your system uses kernel 6.x,
 * please install Ubuntu 20.04.6 LTS (Focal Fossa) as specified in the assignment instructions.
 */

 #include <linux/init.h>           // Required for the module initialization and cleanup macros
 #include <linux/module.h>         // Required for all kernel modules
 #include <linux/kernel.h>         // Needed for KERN_INFO and printk()
 #include <linux/sched/signal.h>   // Needed for for_each_process and access to task_struct
 #include <linux/mm.h>             // Needed for accessing mm_struct and vm_area_struct
 #include <linux/pid_namespace.h>  // Required for working with process IDs in kernel space
 #include <asm/io.h>               // Needed for IO operations and PAGE_MASK
 #include <asm/pgtable.h>          // Required for page table macros and types
 #include <linux/highmem.h>        // Needed for pte_offset_map and pte_unmap functions
 
 MODULE_LICENSE("GPL");            // License type -- this module is released under the GPL
 MODULE_AUTHOR("Your Name");       // Replace with your name
 MODULE_DESCRIPTION("Linux Kernel Module for Process Report with Page Table Walker");  // Module description
 MODULE_VERSION("1.0");            // Module version
 
 /*
  * Function: virt2phys
  * -------------------
  * Translates a virtual address to a physical address for a given memory descriptor (mm_struct).
  *
  * Parameters:
  *    mm   - pointer to the memory descriptor of a process
  *    vaddr- the virtual address to translate
  *
  * Returns:
  *    The physical address corresponding to vaddr if it is valid and allocated;
  *    returns 0 if the virtual address is not mapped to physical memory.
  *
  * The function performs a 5-level page table walk (using pgd, p4d, pud, pmd, and pte)
  * to obtain the physical address. It uses pte_offset_map() to temporarily map the page table
  * entry and then unmaps it using pte_unmap().
  */
 static unsigned long virt2phys(struct mm_struct *mm, unsigned long vaddr)
 {
     pgd_t *pgd;         // Page Global Directory pointer
     p4d_t *p4d;         // Page 4th-level Directory pointer
     pud_t *pud;         // Page Upper Directory pointer
     pmd_t *pmd;         // Page Middle Directory pointer
     pte_t *pte;         // Page Table Entry pointer
     unsigned long phys = 0; // Variable to store the physical address
 
     // Get the PGD entry for the given virtual address from the memory descriptor
     pgd = pgd_offset(mm, vaddr);
     if (pgd_none(*pgd) || pgd_bad(*pgd))
         return 0;   // Return 0 if the PGD entry is not valid
 
     // Get the P4D entry corresponding to vaddr
     p4d = p4d_offset(pgd, vaddr);
     if (p4d_none(*p4d) || p4d_bad(*p4d))
         return 0;   // Return 0 if the P4D entry is not valid
 
     // Get the PUD entry from the P4D entry
     pud = pud_offset(p4d, vaddr);
     if (pud_none(*pud) || pud_bad(*pud))
         return 0;   // Return 0 if the PUD entry is not valid
 
     // Get the PMD entry from the PUD entry
     pmd = pmd_offset(pud, vaddr);
     if (pmd_none(*pmd) || pmd_bad(*pmd))
         return 0;   // Return 0 if the PMD entry is not valid
 
     // Map the PTE for the given virtual address; this creates a temporary kernel mapping
     pte = pte_offset_map(pmd, vaddr);
     if (!pte)
         return 0;   // Return 0 if the PTE mapping failed
 
     // Check if the page table entry is present; if not, unmap and return 0
     if (!pte_present(*pte)) {
         pte_unmap(pte);
         return 0;
     }
 
     // Calculate the physical address:
     // Mask the page table entry with PAGE_MASK and add the offset within the page.
     phys = (pte_val(*pte) & PAGE_MASK) | (vaddr & ~PAGE_MASK);
 
     // Unmap the temporary PTE mapping
     pte_unmap(pte);
     return phys;  // Return the resulting physical address
 }
 
 /*
  * Function: procReport_init
  * -------------------------
  * Module initialization function that is called when the module is loaded.
  *
  * This function iterates over all running processes using the for_each_process macro.
  * For each process with PID > 650 and a valid memory descriptor, it walks through its VMAs
  * and for each page (in increments of PAGE_SIZE) it translates the virtual address to a
  * physical address using virt2phys(). Valid (nonzero) translations are counted.
  *
  * For extra credit, the code also checks whether consecutive valid pages are contiguous in
  * physical memory. The first valid page of a process is counted as non-contiguous by default.
  *
  * The report is printed to the kernel log in CSV format with the following columns:
  *   proc_id, proc_name, total_pages, contig_pages, noncontig_pages
  *
  * Optionally, after processing all processes, a totals line is printed.
  *
  * Returns:
  *    0 on success.
  */
 static int __init procReport_init(void)
 {
     struct task_struct *task;         // Pointer to iterate through task_struct entries
     struct mm_struct *mm;             // Pointer to a process's memory descriptor
     struct vm_area_struct *vma;       // Pointer to a virtual memory area in a process
     unsigned long addr;               // Iterator for addresses within a VMA
     unsigned long phys;               // Physical address corresponding to a virtual address
     unsigned long proc_total_pages;   // Total pages allocated for the current process
     unsigned long proc_contig_pages;  // Count of contiguous pages for the current process
     unsigned long proc_noncontig_pages; // Count of non-contiguous pages for the current process
     unsigned long grand_total_pages = 0;   // Grand total of pages for all processes (PID > 650)
     unsigned long grand_contig_pages = 0;  // Grand total of contiguous pages
     unsigned long grand_noncontig_pages = 0; // Grand total of non-contiguous pages
     unsigned long prev_phys;          // To hold previous page's physical address for contiguity check
     bool first_page;                  // Flag to indicate the first valid page for a process
 
     // Print the report header to the kernel log
     printk(KERN_INFO "PROCESS REPORT:");
     printk(KERN_INFO "proc_id,proc_name,total_pages,contig_pages,noncontig_pages");
 
     // Iterate over all processes in the system
     for_each_process(task) {
         // Only consider processes with a PID greater than 650
         if (task->pid > 650) {
             // Initialize counts for the current process
             proc_total_pages = 0;
             proc_contig_pages = 0;
             proc_noncontig_pages = 0;
             first_page = true;
 
             // Access the process's memory descriptor; kernel threads may have a NULL mm
             mm = task->mm;
             if (mm) {
                 // Iterate over each virtual memory area (VMA) of the process
                 for (vma = mm->mmap; vma; vma = vma->vm_next) {
                     // Loop through the VMA range in steps of PAGE_SIZE
                     for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
                         // Translate the virtual address to a physical address
                         phys = virt2phys(mm, addr);
                         // If the physical address is 0, the page is not allocated in physical memory
                         if (phys == 0)
                             continue;
 
                         // Increment the total page count for the process
                         proc_total_pages++;
 
                         // For the first valid page, mark it as non-contiguous by default
                         if (first_page) {
                             proc_noncontig_pages++;
                             first_page = false;
                         } else {
                             // If the current physical address is contiguous with the previous page
                             if (prev_phys + PAGE_SIZE == phys)
                                 proc_contig_pages++;  // Count as contiguous
                             else
                                 proc_noncontig_pages++;  // Otherwise, count as non-contiguous
                         }
                         // Save the current physical address for the next iteration
                         prev_phys = phys;
                     }
                 }
             }
 
             // Accumulate the counts into the grand totals for all qualifying processes
             grand_total_pages += proc_total_pages;
             grand_contig_pages += proc_contig_pages;
             grand_noncontig_pages += proc_noncontig_pages;
 
             // Print the process report in CSV format:
             // process ID, process name (comm field), total allocated pages,
             // contiguous pages count, and non-contiguous pages count.
             printk(KERN_INFO "%d,%s,%lu,%lu,%lu", task->pid, task->comm, proc_total_pages, proc_contig_pages, proc_noncontig_pages);
         }
     }
 
     // Optionally, print a totals summary row to the kernel log.
     printk(KERN_INFO "TOTALS,,%lu,%lu,%lu", grand_total_pages, grand_contig_pages, grand_noncontig_pages);
 
     return 0;  // Return success
 }
 
 /*
  * Function: procReport_exit
  * -------------------------
  * Module cleanup function that is called when the module is removed.
  *
  * This function simply logs that the procReport module has been unloaded.
  */
 static void __exit procReport_exit(void)
 {
     printk(KERN_INFO "procReport module unloaded.");
 }
 
 // Macros to define the initialization and cleanup functions
 module_init(procReport_init);
 module_exit(procReport_exit);
 
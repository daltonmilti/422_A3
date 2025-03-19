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

 #include <linux/module.h>       // Core kernel module definitions
 #include <linux/kernel.h>       // For printk logging macros
 #include <linux/init.h>         // For __init and __exit macros
 #include <linux/sched.h>        // For task_struct and process iteration
 #include <linux/sched/signal.h> // For for_each_process()
 #include <linux/mm.h>           // For mm_struct and vm_area_struct
 #include <linux/mm_types.h>     // For memory structure types
 #include <linux/pid_namespace.h> // For PID namespace support
 #include <asm/io.h>             // For PAGE_MASK and I/O operations
 #include <asm/pgtable.h>        // For page table related functions/macros
 #include <linux/highmem.h>      // For pte_offset_map() and pte_unmap()
 
 MODULE_LICENSE("GPL");
 MODULE_AUTHOR("Your Name");
 MODULE_DESCRIPTION("Kernel module that reports allocated physical pages per process");
 MODULE_VERSION("0.1");
 
 /**
  * virt2phys - Convert a virtual page address to its corresponding physical address
  * @mm:  Pointer to the memory map of the process
  * @vaddr: Virtual address to be translated
  *
  * Returns the physical address if the page is present; otherwise returns 0.
  */
 unsigned long virt2phys(struct mm_struct *mm, unsigned long vaddr)
 {
     pgd_t *pgd_entry;
     p4d_t *p4d_entry;
     pud_t *pud_entry;
     pmd_t *pmd_entry;
     pte_t *pte_entry;
     struct page *page_ptr;
 
     /* Retrieve the first level (PGD) entry */
     pgd_entry = pgd_offset(mm, vaddr);
     if (pgd_none(*pgd_entry) || pgd_bad(*pgd_entry))
         return 0;
 
     /* Retrieve the 4th-level (P4D) entry */
     p4d_entry = p4d_offset(pgd_entry, vaddr);
     if (p4d_none(*p4d_entry) || p4d_bad(*p4d_entry))
         return 0;
 
     /* Retrieve the upper-level (PUD) entry */
     pud_entry = pud_offset(p4d_entry, vaddr);
     if (pud_none(*pud_entry) || pud_bad(*pud_entry))
         return 0;
 
     /* Retrieve the middle-level (PMD) entry */
     pmd_entry = pmd_offset(pud_entry, vaddr);
     if (pmd_none(*pmd_entry) || pmd_bad(*pmd_entry))
         return 0;
 
     /* Map the page table entry (PTE) into kernel space */
     pte_entry = pte_offset_map(pmd_entry, vaddr);
     if (!pte_entry)
         return 0;
 
     /* Check if the PTE corresponds to a valid physical page */
     page_ptr = pte_page(*pte_entry);
     if (!page_ptr) {
         pte_unmap(pte_entry);
         return 0;
     }
 
     /* Obtain the physical address from the page structure */
     unsigned long phys_addr = page_to_phys(page_ptr);
     pte_unmap(pte_entry);
 
     /* Optional: Check for a special unmapped value (if required by your environment) */
     if (phys_addr == 70368744173568ULL)
         return 0;
 
     return phys_addr;
 }
 
 /**
  * count_allocated_pages - Count physical pages for a given process.
  * @task: Pointer to the process's task_struct.
  * @total: Pointer to the total allocated pages count.
  * @contig: Pointer to count of contiguous pages.
  * @noncontig: Pointer to count of non-contiguous pages.
  *
  * This function iterates over each virtual memory area (VMA) in the process's
  * memory map and, for every page, it attempts to translate the virtual address to a
  * physical address. If successful, it updates the page counts accordingly.
  */
 void count_allocated_pages(struct task_struct *task, unsigned long *total,
                            unsigned long *contig, unsigned long *noncontig)
 {
     struct vm_area_struct *area;
     unsigned long vaddr;
     unsigned long prev_phys = 0;
 
     *total = 0;
     if (contig)
         *contig = 0;
     if (noncontig)
         *noncontig = 0;
 
     if (task->mm && task->mm->mmap) {
         for (area = task->mm->mmap; area; area = area->vm_next) {
             for (vaddr = area->vm_start; vaddr < area->vm_end; vaddr += PAGE_SIZE) {
                 unsigned long phys = virt2phys(task->mm, vaddr);
                 if (phys == 0)
                     continue;
 
                 (*total)++;
                 if (contig && noncontig) {
                     if (prev_phys != 0) {
                         if (phys == prev_phys + PAGE_SIZE)
                             (*contig)++;
                         else
                             (*noncontig)++;
                     }
                     prev_phys = phys;
                 }
             }
         }
     }
 
     /* Adjust for the first page, which has no predecessor */
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
     struct task_struct *proc;
     unsigned long proc_total, proc_contig, proc_noncontig;
     unsigned long grand_total = 0, grand_contig = 0, grand_noncontig = 0;
 
     printk(KERN_INFO "PROCESS REPORT:\n");
     printk(KERN_INFO "proc_id,proc_name,contig_pages,noncontig_pages,total_pages\n");
 
     for_each_process(proc) {
         if (proc->pid > 650) {
             count_allocated_pages(proc, &proc_total, &proc_contig, &proc_noncontig);
             printk(KERN_INFO "%d,%s,%lu,%lu,%lu\n",
                    proc->pid, proc->comm, proc_contig, proc_noncontig, proc_total);
             grand_total += proc_total;
             grand_contig += proc_contig;
             grand_noncontig += proc_noncontig;
         }
     }
 
     printk(KERN_INFO "TOTALS,,%lu,%lu,%lu\n",
            grand_contig, grand_noncontig, grand_total);
 }
 
 /**
  * helloModule_init - Module initialization routine.
  *
  * Logs the start of the module, generates the process report, and confirms successful loading.
  */
 static int __init helloModule_init(void)
 {
     printk(KERN_INFO "helloModule: Initializing module...\n");
     generate_report();
     printk(KERN_INFO "helloModule: Module loaded successfully.\n");
     return 0;
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
 
 module_init(helloModule_init);
 module_exit(helloModule_exit);
 
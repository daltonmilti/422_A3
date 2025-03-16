#include <linux/module.h>         // Needed for all kernel modules
#include <linux/init.h>           // Needed for module init and exit macros
#include <linux/sched/signal.h>   // Provides for_each_process and task_struct
#include <linux/mm.h>           // Provides mm_struct and vma
#include <asm/pgtable.h>          // For page table functions and macros

// Helper function to translate a virtual address to its physical address.
// Returns 0 if the page is not present.
static phys_addr_t virt2phys(struct mm_struct *mm, unsigned long vaddr)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    phys_addr_t phys = 0;

    pgd = pgd_offset(mm, vaddr);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
        return 0;

    p4d = p4d_offset(pgd, vaddr);
    if (p4d_none(*p4d) || p4d_bad(*p4d))
        return 0;

    pud = pud_offset(p4d, vaddr);
    if (pud_none(*pud) || pud_bad(*pud))
        return 0;

    pmd = pmd_offset(pud, vaddr);
    if (pmd_none(*pmd) || pmd_bad(*pmd))
        return 0;

    pte = pte_offset_map(pmd, vaddr);
    if (!pte)
        return 0;

    if (!pte_present(*pte)) {
        pte_unmap(pte);
        return 0;
    }
    phys = (phys_addr_t)pte_pfn(*pte) << PAGE_SHIFT;
    pte_unmap(pte);
    return phys;
}

// Module initialization function: iterates processes and prints a report.
static int proc_init(void)
{
    struct task_struct *task;
    unsigned long global_total_pages = 0, global_contig_pages = 0, global_noncontig_pages = 0;

    printk(KERN_INFO "PROCESS REPORT:\n");
    printk(KERN_INFO "proc_id,proc_name,total_pages,contig_pages,noncontig_pages\n");

    for_each_process(task) {
        int total_pages = 0;
        int contig_pages = 0;
        int noncontig_pages = 0;
        unsigned long prev_phys = 0;
        int first_page = 1;

        // Only process tasks with PID > 650 and with a valid memory map.
        if (task->pid <= 650)
            continue;
        if (task->mm == NULL)
            continue;

        // Lock the memory map of the process for safe traversal.
        down_read(&task->mm->mmap_sem);
        {
            struct vm_area_struct *vma;
            for (vma = task->mm->mmap; vma; vma = vma->vm_next) {
                unsigned long addr;
                // Iterate through each page in the VMA.
                for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
                    phys_addr_t phys = virt2phys(task->mm, addr);
                    if (phys == 0)
                        continue;  // Skip if page is not allocated in physical memory.

                    total_pages++;

                    // Check for contiguous allocation.
                    if (first_page) {
                        first_page = 0;
                    } else {
                        if (phys == prev_phys + PAGE_SIZE)
                            contig_pages++;
                        else
                            noncontig_pages++;
                    }
                    prev_phys = phys;
                }
            }
        }
        up_read(&task->mm->mmap_sem);

        // If only one page is allocated, count it as non-contiguous (no comparison made).
        if (total_pages == 1)
            noncontig_pages = 1;

        // Sum up for a global totals line.
        global_total_pages += total_pages;
        global_contig_pages += contig_pages;
        global_noncontig_pages += noncontig_pages;

        // Print the process report line in CSV format.
        printk(KERN_INFO "%d,%s,%d,%d,%d\n",
               task->pid,      // Process ID
               task->comm,     // Process name
               total_pages,
               contig_pages,
               noncontig_pages);
    }

    // Optional: print a final totals line.
    printk(KERN_INFO "TOTALS,,%lu,%lu,%lu\n",
           global_total_pages,
           global_contig_pages,
           global_noncontig_pages);

    return 0;
}

// Module cleanup function: simply logs a message on module removal.
static void proc_cleanup(void)
{
    printk(KERN_INFO "procReport: Module cleanup\n");
}

MODULE_LICENSE("GPL");
module_init(proc_init);
module_exit(proc_cleanup);

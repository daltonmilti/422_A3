#include <linux/module.h>         // Needed for all kernel modules
#include <linux/init.h>           // Needed for module init and exit macros
#include <linux/sched/signal.h>   // Provides for_each_process and task_struct
#include <linux/mm.h>             // Provides mm_struct and vma
#include <asm/pgtable.h>          // For page table functions and macros

// Helper function to translate a virtual address to its physical address.
// Returns 0 if the page is not present (unmapped).
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

static int proc_init(void)
{
    struct task_struct *task;
    unsigned long global_total_pages = 0;
    unsigned long global_contig_pages = 0;
    unsigned long global_noncontig_pages = 0;

    printk(KERN_INFO "PROCESS REPORT:\n");
    printk(KERN_INFO "proc_id,proc_name,total_pages,contig_pages,noncontig_pages\n");

    for_each_process(task) {
        // Only process tasks with PID > 650 and a valid memory map.
        if (task->pid <= 650)
            continue;
        if (task->mm == NULL)
            continue;

        // Lock the memory map to safely traverse VMAs.
        down_read(&task->mm->mmap_sem);

        {
            struct vm_area_struct *vma;
            int total_pages = 0;
            int contig_pages = 0;
            int noncontig_pages = 0;

            for (vma = task->mm->mmap; vma; vma = vma->vm_next) {
                unsigned long addr;
                int first_page = 1;
                unsigned long prev_phys = 0;

                // Walk each page in this VMA.
                for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
                    phys_addr_t phys = virt2phys(task->mm, addr);
                    if (phys == 0)
                        continue; // Skip unmapped pages.

                    // Found a physically backed page.
                    total_pages++;

                    // Check if contiguous with previous allocated page.
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

            up_read(&task->mm->mmap_sem);

            /*
             * By default, contig_pages + noncontig_pages = total_pages - 1
             * (for multi-page processes). If total_pages > 0, add 1 to
             * noncontig_pages so that contig_pages + noncontig_pages == total_pages.
             * This treats the "first page" as automatically non-contiguous.
             */
            if (total_pages > 0)
                noncontig_pages++;

            // Accumulate in global totals.
            global_total_pages += total_pages;
            global_contig_pages += contig_pages;
            global_noncontig_pages += noncontig_pages;

            // Print CSV line for this process.
            printk(KERN_INFO "%d,%s,%d,%d,%d\n",
                   task->pid,
                   task->comm,
                   total_pages,
                   contig_pages,
                   noncontig_pages);
        }
    }

    // Print optional totals line.
    printk(KERN_INFO "TOTALS,,%lu,%lu,%lu\n",
           global_total_pages,
           global_contig_pages,
           global_noncontig_pages);

    return 0;
}

static void proc_cleanup(void)
{
    printk(KERN_INFO "procReport: Module cleanup\n");
}

MODULE_LICENSE("GPL");
module_init(proc_init);
module_exit(proc_cleanup);

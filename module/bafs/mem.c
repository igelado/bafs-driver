#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/dma-mapping.h>

#include <nv-p2p.h>

#include <linux/bafs.h>

#include <linux/bafs/util.h>
#include <linux/bafs/types.h>


static 
int pin_bafs_cpu_mem(struct bafs_mem* mem, struct vm_area_struct* vma)
{
    int ret = 0;
    int count;
    int i;
    UNUSED(vma);

    mem->page_size                     = PAGE_SIZE;
    mem->page_shift                    = PAGE_SHIFT;
    mem->n_pages                       = (mem->size + mem->page_size - 1) >> mem->page_shift;
    mem->page_mask                     = ~(mem->page_size - 1);
    if ((mem->vaddr & mem->page_mask) != mem->vaddr) {
        ret                            = -EINVAL;
        goto out;
    }
    mem->cpu_page_table = (struct page**) kcalloc(mem->n_pages, sizeof(struct page*), GFP_KERNEL);
    if (!mem->cpu_page_table){
        ret             = -ENOMEM;
        BAFS_CORE_DEBUG("Failed to pin cpu memory due to lack of memory\n");
        goto out;
    }


    ret = get_user_pages(mem->vaddr, mem->n_pages, FOLL_WRITE, mem->cpu_page_table, NULL);


    if (ret <= 0) {
        ret = -ENOMEM;
        BAFS_CORE_DEBUG("Failed to pin cpu memory due to get_user_pages failure\n");
        goto out_delete_page_table;
    }

    if ((ret > 0) && (ret < mem->n_pages)) {
        count = ret;
        BAFS_CORE_DEBUG("Failed to pin cpu memory due get_user_pages only getting %i pages when %lu were requested\n", count, mem->n_pages);
        ret   = -ENOMEM;
        goto out_clean_page_table;
    }

    ret = 0;
    return ret;

out_clean_page_table:
    for (i = 0; i < count; i++)
        put_page(mem->cpu_page_table[i]);
out_delete_page_table:
    kfree(mem->cpu_page_table);
    mem->cpu_page_table = NULL;
out:
    return ret;
}


static 
void __bafs_mem_release_cuda(struct kref* ref)
{
    struct bafs_mem*      mem;
    struct bafs_core_ctx* ctx;

    mem     = container_of(ref, struct bafs_mem, ref);
    if (mem) {
        ctx = mem->ctx;
        spin_lock(&ctx->lock);
        list_del(&mem->mem_list);
        xa_erase(&ctx->bafs_mem_xa, mem->mem_id);
        spin_unlock(&ctx->lock);


        if (mem->cuda_page_table)
            nvidia_p2p_free_page_table(mem->cuda_page_table);

        kfree_rcu(mem, rh);
        bafs_put_ctx(ctx);
    }
}

static
void release_bafs_cuda_mem(void* data)
{
    struct bafs_mem* mem;

    struct bafs_mem_dma* dma;
    struct bafs_mem_dma* next;

    mem = (struct bafs_mem*) data;


    spin_lock(&mem->lock);

    list_for_each_entry_safe(dma, next, &mem->dma_list, dma_list) {
        if (dma->cuda_mapping)
            nvidia_p2p_free_dma_mapping(dma->cuda_mapping);
        dma->cuda_mapping = NULL;
        list_del(&dma->dma_list);
        kfree_rcu(dma, rh);
        bafs_ctrl_release(dma->ctrl);
        kref_put(&mem->ref, __bafs_mem_release_cuda);
    }
    if (mem->cuda_page_table)
        nvidia_p2p_free_page_table(mem->cuda_page_table);
    mem->cuda_page_table = NULL;
    mem->state           = DEAD;
    spin_unlock(&mem->lock);


    kref_put(&mem->ref, __bafs_mem_release_cuda);


}

static
int pin_bafs_cuda_mem(struct bafs_mem* mem, struct vm_area_struct* vma)
{
    int      ret = 0;
    int      i;
    unsigned map_gran;

    ret = nvidia_p2p_get_pages(0, 0, mem->vaddr, mem->size, &mem->cuda_page_table,
                               release_bafs_cuda_mem, mem);
    if(ret < 0) {
        goto out;
    }


    if(!NVIDIA_P2P_PAGE_TABLE_VERSION_COMPATIBLE(mem->cuda_page_table)){
        ret = -EFAULT;
        BAFS_CORE_DEBUG("Failed to pin cuda memory due to incompatible page table version\n");
        goto out_delete_page_table;
    }

    switch (mem->cuda_page_table->page_size) {
    case NVIDIA_P2P_PAGE_SIZE_4KB:
        mem->page_size  = 4*1024;
        mem->page_shift = 12;
        break;
    case NVIDIA_P2P_PAGE_SIZE_64KB:
        mem->page_size  = 64*1024;
        mem->page_shift = 16;
        break;
    case NVIDIA_P2P_PAGE_SIZE_128KB:
        mem->page_size  = 128*1024;
        mem->page_shift = 17;
        break;
    default:
        ret             = -EINVAL;
        BAFS_CORE_DEBUG("Failed to pin cuda memory due to invalid page size\n");
        goto out_delete_page_table;

    }

    mem->page_mask = ~(mem->page_size - 1);


    if ((mem->vaddr & mem->page_mask) != mem->vaddr) {
        ret                            = -EINVAL;
        BAFS_CORE_DEBUG("Failed to pin cuda memory due to unaligned vaddr\n");
        goto out_delete_page_table;
    }

     mem->n_pages = (mem->size + mem->page_size - 1) >> mem->page_size;

    if (mem->n_pages != mem->cuda_page_table->entries) {
        ret = -ENOMEM;
        BAFS_CORE_DEBUG("Failed to pin cuda memory due to unavailable pages\n");
        goto out_delete_page_table;
    }

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    for (i             = 0; i < mem->n_pages; i++) {
        map_gran = mem->page_size;
        if ((i*mem->page_size) > mem->size) {
            map_gran  -= ((i*mem->page_size) - mem->size);
        }
        ret = io_remap_pfn_range(vma, mem->vaddr + (i*mem->page_size),
                                 __phys_to_pfn(mem->cuda_page_table->pages[i]->physical_address),
                                 map_gran, vma->vm_page_prot);
        if (!ret) {
            BAFS_CORE_DEBUG("Failed to pin cuda memory due to failure to map cuda memory to process address space\n");
            goto out_delete_page_table;
        }
    }

    ret = 0;
    return ret;

out_delete_page_table:
    nvidia_p2p_put_pages(0, 0, mem->vaddr, mem->cuda_page_table);
    mem->cuda_page_table = NULL;
out:
    return ret;
}

static
void __bafs_mem_release(struct kref* ref)
{
    struct bafs_mem*      mem;
    struct bafs_core_ctx* ctx;

    int i;

    mem     = container_of(ref, struct bafs_mem, ref);
    if (mem) {
        ctx = mem->ctx;
        spin_lock(&ctx->lock);
        list_del(&mem->mem_list);
        xa_erase(&ctx->bafs_mem_xa, mem->mem_id);
        spin_unlock(&ctx->lock);

        spin_lock(&mem->lock);

        switch (mem->loc) {
        case BAFS_MEM_CPU:
            if (mem->cpu_page_table) {
                for (i = 0; i < mem->n_pages; i++)
                    put_page(mem->cpu_page_table[i]);
                kfree(mem->cpu_page_table);
            }
            break;
        case BAFS_MEM_CUDA:
            if (mem->cuda_page_table)
                nvidia_p2p_put_pages(0, 0, mem->vaddr, mem->cuda_page_table);
            break;
        default:
            break;

        }
        mem->state = DEAD;
        spin_unlock(&mem->lock);
        kfree_rcu(mem, rh);

        bafs_put_ctx(ctx);
    }
}

void
bafs_mem_put(struct bafs_mem * mem)
{
    kref_put(&mem->ref, __bafs_mem_release);
}

long bafs_core_reg_mem(void __user* user_params, struct bafs_core_ctx* ctx)
{
    long ret = 0;

    struct bafs_mem*                    mem;
    struct BAFS_CORE_IOC_REG_MEM_PARAMS params;

    if (copy_from_user(&params, user_params, sizeof(params))) {
        ret = -EFAULT;
        BAFS_CORE_ERR("Failed to copy params from user\n");
        goto out;
    }

    mem     = kzalloc(sizeof(*mem), GFP_KERNEL);
    if (!mem) {
        ret = -ENOMEM;
        BAFS_CORE_ERR("Failed to allocate memory for bafs_mem\n");
        goto out;
    }

    kref_get(&ctx->ref);
    mem->state = STALE;

    mem->size = params.size;
    mem->loc  = params.loc;
    mem->ctx  = ctx;
    INIT_LIST_HEAD(&mem->mem_list);

    spin_lock(&ctx->lock);
    ret     = xa_alloc(&ctx->bafs_mem_xa, &(params.handle), mem, xa_limit_32b, GFP_KERNEL);
    if (ret < 0) {
        ret = -ENOMEM;
        BAFS_CORE_ERR("Failed to allocate entry in bafs_mem_xa\n");
        goto out_delete_mem;
    }
    mem->mem_id = params.handle;

    list_add(&mem->mem_list, &ctx->mem_list);
    spin_unlock(&ctx->lock);


    if (copy_to_user(user_params, &params, sizeof(params))) {
        ret = -EFAULT;
        BAFS_CORE_ERR("Failed to copy params to user\n");
        goto out_erase_xa_entry;
    }


    ret = 0;
    return ret;

out_erase_xa_entry:


    spin_lock(&ctx->lock);
    list_del_init(&mem->mem_list);
    xa_erase(&ctx->bafs_mem_xa, params.handle);

out_delete_mem:
    spin_unlock(&ctx->lock);
    bafs_put_ctx(ctx);
    kfree(mem);
out:
    return ret;
}


void unmap_dma(struct bafs_mem_dma* dma)
{
    unsigned map_gran;
    int      i;

    struct bafs_mem* mem;
    struct pci_dev*  pdev;

    mem                  = dma->mem;
    list_del(&dma->dma_list);
    switch (mem->loc) {
    case BAFS_MEM_CPU:
        if (dma->addrs) {
            for (i = 0; i < mem->n_pages; i++) {
                map_gran = dma->map_gran;
                if ((i*dma->map_gran) > mem->size) {
                    map_gran -= ((i*dma->map_gran) - mem->size);
                }
                dma_unmap_single(dma->ctrl->dev, dma->addrs[i], map_gran, DMA_BIDIRECTIONAL);
            }

            kfree(dma->addrs);
            dma->addrs = NULL;
        }
        break;
    case BAFS_MEM_CUDA:
        if (dma->cuda_mapping && mem->cuda_page_table) {
            nvidia_p2p_dma_unmap_pages(dma->ctrl->pdev, mem->cuda_page_table, dma->cuda_mapping);
            //dma->cuda_mapping = NULL;
        }
        break;
    default:
        break;

    }

    pdev = dma->ctrl->pdev;

    kfree_rcu(dma, rh);
    bafs_ctrl_release(dma->ctrl);
}

static
void bafs_mem_release(struct vm_area_struct* vma)
{
    struct bafs_mem*      mem;
    struct bafs_core_ctx* ctx;
    struct bafs_mem_dma*  dma;
    struct bafs_mem_dma*  next;


    mem     = (struct bafs_mem*) vma->vm_private_data;
    if (!mem) {
        goto out;
    }

    ctx = mem->ctx;
    kref_get(&ctx->ref);

    spin_lock(&mem->lock);
    list_for_each_entry_safe(dma, next, &mem->dma_list, dma_list) {
        unmap_dma(dma);
        kref_put(&mem->ref, __bafs_mem_release);
    }
    mem->state = DEAD;

    spin_unlock(&mem->lock);

    vma->vm_private_data = NULL;
    kref_put(&mem->ref, __bafs_mem_release);
    bafs_put_ctx(ctx);



out:
    return;
}

const struct vm_operations_struct bafs_mem_ops = {
    .close = bafs_mem_release,
};


int pin_bafs_mem(struct vm_area_struct* vma, struct bafs_core_ctx* ctx)
{
    int ret = 0;

    struct bafs_mem* mem;
    bafs_mem_hnd_t   mem_id;

    mem_id = vma->vm_pgoff;
    kref_get(&ctx->ref);
    spin_lock(&ctx->lock);
    mem    = (struct bafs_mem*) xa_load(&ctx->bafs_mem_xa, mem_id);
    spin_unlock(&ctx->lock);
    bafs_put_ctx(ctx);

    if (!mem) {
        ret = -EINVAL;
        goto out;
    }
    kref_get(&mem->ref);
    spin_lock(&mem->lock);

    mem->vaddr = vma->vm_start;

    vma->vm_flags |= VM_DONTCOPY;
    vma->vm_flags |= VM_DONTEXPAND;

    switch (mem->loc) {

    case BAFS_MEM_CPU:
        ret = pin_bafs_cpu_mem(mem, vma);
        if (!ret)
            goto out_release;
        break;

    case BAFS_MEM_CUDA:
        ret = pin_bafs_cuda_mem(mem, vma);
        if (!ret)
            goto out_release;
        break;

    default:
        spin_lock(&mem->lock);
        ret = -EINVAL;
        return ret;
        break;
    }

    vma->vm_ops          = &bafs_mem_ops;
    mem->state           = LIVE;
    vma->vm_private_data = mem;

    ret = 0;
out_release:
    spin_unlock(&mem->lock);
    kref_put(&mem->ref, __bafs_mem_release);
out:
    return ret;
}





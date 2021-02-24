#ifndef __BAFS_CTRL_H__
#define __BAFS_CTRL_H__

#include "bafs_util.h"
#include "bafs_mem.h"
#include "bafs_ctrl_ioctl.h"
#include "bafs_types.h"
#include "bafs_release.h"




int __bafs_ctrl_dma_map_mem(struct bafs_ctrl* ctrl, addr_ vaddr, unsigned long* n_dma_addrs,  addr_ __user* dma_addrs_user, struct bafs_mem_dma** dma_, const int ctrl_id) {
    int ret = 0;
    int i   = 0;

    struct bafs_mem*       mem;
    struct vm_area_struct* vma;
    struct bafs_mem_dma*   dma;
    unsigned               map_gran;


    vma     = find_vma(current->mm, vaddr);
    if (!vma) {
        ret = -EINVAL;
        goto out;
    }
    mem     = (struct bafs_mem*) vma->vm_private_data;
    if (!mem) {
        ret = -EINVAL;
        goto out;
    }


    *dma_   = kzalloc(sizeof(*dma), GFP_KERNEL);
    if (!(*dma_)){
        ret = -ENOMEM;
        BAFS_CTRL_ERR("Failed to allocate memory for bafs_mem_dma\n");
        goto out;
    }

    dma = *dma_;

    kref_get(&mem->ref);

    dma->dev = bafs_get_ctrl(ctrl);

    INIT_LIST_HEAD(&dma->dma_list);


    dma->ctrl = ctrl;

    spin_lock(&mem->lock);
    list_add(&dma->dma_list, &mem->dma_list);
    spin_unlock(&mem->lock);




    switch (mem->loc) {
    case CPU:
        dma->map_gran = mem->page_size;
        dma->addrs    = (addr_*) kcalloc(mem->n_pages, sizeof(addr_*), GFP_KERNEL);
        if (!dma->addrs) {
            ret       = -ENOMEM;
            goto out_delete_mem;
        }
        for (i = 0; i < mem->n_pages; i++) {
            map_gran      = dma->map_gran;
            if ((i*dma->map_gran) > mem->size) {
                map_gran -= ((i*dma->map_gran) - mem->size);
            }
            dma->addrs[i] = dma_map_single(dma->ctrl->dev, page_to_virt(mem->cpu_page_table[i]), map_gran, DMA_BIDIRECTIONAL);
            if (dma_mapping_error(dma->ctrl->dev, dma->addrs[i])) {
                ret       = -EFAULT;
                goto out_unmap;
            }
        }
        if (ctrl_id      == 0)
            *n_dma_addrs  = mem->n_pages;
        else
            *n_dma_addrs += mem->n_pages;


        if (copy_to_user(dma_addrs_user, dma->addrs, (*n_dma_addrs)*sizeof(addr_))) {
            ret = -EFAULT;
            BAFS_CTRL_ERR("Failed to copy dma addrs to user\n");
            goto out_unmap;
        }

        break;
    case CUDA:
        ret      = nvidia_p2p_dma_map_pages(ctrl->pdev, mem->cuda_page_table, &dma->cuda_mapping);
        if (ret != 0) {
            goto out_delete_mem;
        }
        if (ctrl_id      == 0)
            *n_dma_addrs  = dma->cuda_mapping->entries;
        else
            *n_dma_addrs += dma->cuda_mapping->entries;


        if (copy_to_user(dma_addrs_user, dma->cuda_mapping->dma_addresses, (*n_dma_addrs)*sizeof(addr_))) {
            ret = -EFAULT;
            BAFS_CTRL_ERR("Failed to copy dma addrs to user\n");
            goto out_unmap;
        }
        break;
    default:
        ret = -EINVAL;
        goto out_delete_mem;
        break;

    }



    return ret;

out_unmap:
    if (mem->loc == CPU) {
        for (i    = i-1; i >= 0; i--)
            dma_unmap_single(dma->ctrl->dev, dma->addrs[i], dma->map_gran, DMA_BIDIRECTIONAL);
    }
    else if(mem->loc == CUDA) {
        nvidia_p2p_dma_unmap_pages(dma->ctrl->pdev, mem->cuda_page_table, dma->cuda_mapping);
    }
out_delete_mem:
    spin_lock(&mem->lock);
    list_del_init(&dma->dma_list);
    spin_unlock(&mem->lock);


    kfree(dma);

    bafs_put_ctrl(ctrl, __bafs_ctrl_release);

    kref_put(&mem->ref, __bafs_mem_release);


out:
    return ret;

} 

void __bafs_ctrl_dma_unmap_mem(struct bafs_mem_dma* dma) {
    struct bafs_mem* mem = dma->mem;
    spin_lock(&mem->lock);
    unmap_dma(dma);
    spin_unlock(&mem->lock);
    kref_put(&mem->ref, __bafs_mem_release);

}

long bafs_ctrl_dma_map_mem(struct bafs_ctrl* ctrl, void __user* user_params) {

    long ret = 0;

    struct bafs_mem_dma*                    dma;
    struct BAFS_CTRL_IOC_DMA_MAP_MEM_PARAMS params = {0};

    if (copy_from_user(&params, user_params, sizeof(params))) {
        ret = -EFAULT;
        BAFS_CTRL_ERR("Failed to copy params from user\n");
        goto out;
    }


    ret = __bafs_ctrl_dma_map_mem(ctrl, params.vaddr, &params.n_dma_addrs, params.dma_addrs, &dma, 0);
    if (ret < 0) {
        goto out;
    }



    if (copy_to_user(user_params, &params, sizeof(params))) {
        ret = -EFAULT;
        BAFS_CTRL_ERR("Failed to copy params to user\n");
        goto out_unmap_memory;
    }


    return ret;
out_unmap_memory:
    __bafs_ctrl_dma_unmap_mem(dma);
out:
    return ret;
}



long bafs_ctrl_ioctl(struct file* file, unsigned int cmd, unsigned long arg) {

    long ret = 0;

    void __user*      argp = (void __user*) arg;
    struct bafs_ctrl* ctrl = file->private_data;

    if (!ctrl) {
        ret = -EINVAL;
        goto out;
    }
    bafs_get_ctrl(ctrl);

    BAFS_CTRL_DEBUG("IOCTL called \t cmd = %u\n", cmd);

    if (_IOC_TYPE(cmd) != BAFS_CTRL_IOCTL) {

        ret = -EINVAL;

        BAFS_CTRL_ERR("Invalid IOCTL commad type  = %u\n", _IOC_TYPE(cmd));
        goto out_release_ctrl;
    }

    switch (cmd) {
    case BAFS_CTRL_IOC_DMA_MAP_MEM:
        ret = bafs_ctrl_dma_map_mem(ctrl, argp);
        if (ret < 0) {
            BAFS_CTRL_ERR("IOCTL to dma map memory failed\n");
            goto out_release_ctrl;
        }
        break;
    default:
        ret                                     = -EINVAL;
        BAFS_CTRL_ERR("Invalid IOCTL cmd \t cmd = %u\n", cmd);
        goto out_release_ctrl;
        break;
    }

    ret = 0;
out_release_ctrl:
    bafs_put_ctrl(ctrl, __bafs_ctrl_release);
out:
    return ret;
}


int bafs_ctrl_open(struct inode* inode, struct file* file) {

    int ret = 0;

    struct bafs_ctrl* ctrl = container_of(inode->i_cdev, struct bafs_ctrl, cdev);

    if (!ctrl) {
        ret = -EINVAL;
        goto out;
    }
    bafs_get_ctrl(ctrl);
    file->private_data = ctrl;
    return ret;
out:
    return ret;
}

int bafs_ctrl_release(struct inode* inode, struct file* file) {

    int ret = 0;

    struct bafs_ctrl* ctrl = (struct bafs_ctrl*) file->private_data;

    if (!ctrl) {
        ret = -EINVAL;
        goto out;
    }
    bafs_put_ctrl(ctrl, __bafs_ctrl_release);
    return ret;
out:
    return ret;
}

int __bafs_ctrl_mmap(struct bafs_ctrl* ctrl, struct vm_area_struct* vma, const unsigned long vaddr, unsigned long* map_size) {
    int ret = 0;

    if (!ctrl) {
        ret = -EINVAL;
        goto out;
    }
    bafs_get_ctrl(ctrl);
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    *map_size         = pci_resource_len(ctrl->pdev, 0);

    ret = io_remap_pfn_range(vma, vaddr, pci_resource_start(ctrl->pdev, 0), *map_size, vma->vm_page_prot);
    if (ret < 0) {
        goto out_put_ctrl;
    }

    return ret;
out_put_ctrl:
    bafs_put_ctrl(ctrl, __bafs_ctrl_release);
out:
    return ret;
}

int bafs_ctrl_mmap(struct file* file, struct vm_area_struct* vma) {
    int ret = 0;

    unsigned long map_size = 0;

    struct bafs_ctrl* ctrl = (struct bafs_ctrl*) file->private_data;

    if (!ctrl) {
        ret = -EINVAL;
        goto out;
    }
    ret = __bafs_ctrl_mmap(ctrl, vma, vma->vm_start, &map_size);
    if (ret < 0) {
        goto out;
    }

    return ret;
out:
    return ret;
}

const struct file_operations bafs_ctrl_fops = {

    .owner          = THIS_MODULE,
    .open           = bafs_ctrl_open,
    .unlocked_ioctl = bafs_ctrl_ioctl,
    .release        = bafs_ctrl_release,
    .mmap           = bafs_ctrl_mmap,

};





#endif                          // __BAFS_CTRL_H__

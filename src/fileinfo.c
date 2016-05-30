/* Copyright (c) Mark Harmstone 2016
 * 
 * This file is part of WinBtrfs.
 * 
 * WinBtrfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 * 
 * WinBtrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with WinBtrfs.  If not, see <http://www.gnu.org/licenses/>. */

#include "btrfs_drv.h"

// static NTSTATUS STDCALL move_subvol(device_extension* Vcb, file_ref* fileref, root* destsubvol, UINT64 destinode, PANSI_STRING utf8, UINT32 crc32,
//                                     UINT32 oldcrc32, BTRFS_TIME* now, BOOL ReplaceIfExists, LIST_ENTRY* rollback);

static NTSTATUS STDCALL set_basic_information(device_extension* Vcb, PIRP Irp, PFILE_OBJECT FileObject) {
    FILE_BASIC_INFORMATION* fbi = Irp->AssociatedIrp.SystemBuffer;
    fcb* fcb = FileObject->FsContext;
    ccb* ccb = FileObject->FsContext2;
    file_ref* fileref = ccb ? ccb->fileref : NULL;
    ULONG defda;
    BOOL inode_item_changed = FALSE;
    NTSTATUS Status;
    
    if (fcb->ads) {
        if (fileref && fileref->parent)
            fcb = fileref->parent->fcb;
        else {
            ERR("stream did not have fileref\n");
            return STATUS_INTERNAL_ERROR;
        }
    }
    
    TRACE("file = %S, attributes = %x\n", file_desc(FileObject), fbi->FileAttributes);
    
    ExAcquireResourceExclusiveLite(fcb->Header.Resource, TRUE);
    
    if (fbi->FileAttributes & FILE_ATTRIBUTE_DIRECTORY && fcb->type != BTRFS_TYPE_DIRECTORY) {
        WARN("attempted to set FILE_ATTRIBUTE_DIRECTORY on non-directory\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }
    
    // FIXME - what if FCB is volume or root?
    // FIXME - what about subvol roots?
    
    // FIXME - link FILE_ATTRIBUTE_READONLY to st_mode
    // FIXME - handle times == -1
    
    // FileAttributes == 0 means don't set - undocumented, but seen in fastfat
    if (fbi->FileAttributes != 0) {
        LARGE_INTEGER time;
        BTRFS_TIME now;
        
        defda = get_file_attributes(Vcb, &fcb->inode_item, fcb->subvol, fcb->inode, fcb->type, fileref->filepart.Length > 0 && fileref->filepart.Buffer[0] == '.', TRUE);
        
        if (fcb->type == BTRFS_TYPE_DIRECTORY)
            fbi->FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
        else if (fcb->type == BTRFS_TYPE_SYMLINK)
            fbi->FileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
                
        fcb->atts_changed = TRUE;
        
        if (defda == fbi->FileAttributes)
            fcb->atts_deleted = TRUE;
        
        fcb->atts = fbi->FileAttributes;
        
        KeQuerySystemTime(&time);
        win_time_to_unix(time, &now);
        
        fcb->inode_item.st_ctime = now;
        fcb->subvol->root_item.ctransid = Vcb->superblock.generation;
        fcb->subvol->root_item.ctime = now;
        
        inode_item_changed = TRUE;
    }
    
//     FIXME - CreationTime
//     FIXME - LastAccessTime
//     FIXME - LastWriteTime
//     FIXME - ChangeTime

    if (inode_item_changed) {
        fcb->inode_item.transid = Vcb->superblock.generation;
        fcb->inode_item.sequence++;
        
        mark_fcb_dirty(fcb);
    }

    Status = STATUS_SUCCESS;

end:
    ExReleaseResourceLite(fcb->Header.Resource);
    
    return Status;
}

static NTSTATUS STDCALL set_disposition_information(device_extension* Vcb, PIRP Irp, PFILE_OBJECT FileObject) {
    FILE_DISPOSITION_INFORMATION* fdi = Irp->AssociatedIrp.SystemBuffer;
    fcb* fcb = FileObject->FsContext;
    ccb* ccb = FileObject->FsContext2;
    file_ref* fileref = ccb ? ccb->fileref : NULL;
    ULONG atts;
    NTSTATUS Status;
    
    if (!fileref)
        return STATUS_INVALID_PARAMETER;
    
    ExAcquireResourceExclusiveLite(fcb->Header.Resource, TRUE);
    
    TRACE("changing delete_on_close to %s for %S (fcb %p)\n", fdi->DeleteFile ? "TRUE" : "FALSE", file_desc(FileObject), fcb);
    
    if (fcb->ads) {
        if (fileref->parent)
            atts = fileref->parent->fcb->atts;
        else {
            ERR("no fileref for stream\n");
            Status = STATUS_INTERNAL_ERROR;
            goto end;
        }
    } else
        atts = fcb->atts;
    
    TRACE("atts = %x\n", atts);
    
    if (atts & FILE_ATTRIBUTE_READONLY) {
        Status = STATUS_CANNOT_DELETE;
        goto end;
    }
    
    // FIXME - can we skip this bit for subvols?
    if (fcb->type == BTRFS_TYPE_DIRECTORY && fcb->inode_item.st_size > 0) {
        Status = STATUS_DIRECTORY_NOT_EMPTY;
        goto end;
    }
    
    if (!MmFlushImageSection(&fcb->nonpaged->segment_object, MmFlushForDelete)) {
        WARN("trying to delete file which is being mapped as an image\n");
        Status = STATUS_CANNOT_DELETE;
        goto end;
    }
    
    ccb->fileref->delete_on_close = fdi->DeleteFile;
    
    FileObject->DeletePending = fdi->DeleteFile;
    
    Status = STATUS_SUCCESS;
    
end:
    ExReleaseResourceLite(fcb->Header.Resource);

    return Status;
}

static NTSTATUS add_inode_extref(device_extension* Vcb, root* subvol, UINT64 inode, UINT64 parinode, UINT64 index, PANSI_STRING utf8, LIST_ENTRY* rollback) {
    KEY searchkey;
    traverse_ptr tp;
    INODE_EXTREF* ier;
    NTSTATUS Status;
    
    searchkey.obj_id = inode;
    searchkey.obj_type = TYPE_INODE_EXTREF;
    searchkey.offset = calc_crc32c((UINT32)parinode, (UINT8*)utf8->Buffer, utf8->Length);

    Status = find_item(Vcb, subvol, &tp, &searchkey, FALSE);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08x\n", Status);
        return Status;
    }
    
    if (!keycmp(&searchkey, &tp.item->key)) {
        ULONG iersize = tp.item->size + sizeof(INODE_EXTREF) - 1 + utf8->Length;
        UINT8* ier2;
        UINT32 maxlen = Vcb->superblock.node_size - sizeof(tree_header) - sizeof(leaf_node);
        
        if (iersize > maxlen) {
            ERR("item would be too long (%u > %u)\n", iersize, maxlen);
            return STATUS_INTERNAL_ERROR;
        }
        
        ier2 = ExAllocatePoolWithTag(PagedPool, iersize, ALLOC_TAG);
        if (!ier2) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        if (tp.item->size > 0)
            RtlCopyMemory(ier2, tp.item->data, tp.item->size);
        
        ier = (INODE_EXTREF*)&ier2[tp.item->size];
        ier->dir = parinode;
        ier->index = index;
        ier->n = utf8->Length;
        RtlCopyMemory(ier->name, utf8->Buffer, utf8->Length);
        
        delete_tree_item(Vcb, &tp, rollback);
        
        if (!insert_tree_item(Vcb, subvol, searchkey.obj_id, searchkey.obj_type, searchkey.offset, ier2, iersize, NULL, rollback)) {
            ERR("error - failed to insert item\n");
            return STATUS_INTERNAL_ERROR;
        }
    } else {
        ier = ExAllocatePoolWithTag(PagedPool, sizeof(INODE_EXTREF) - 1 + utf8->Length, ALLOC_TAG);
        if (!ier) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ier->dir = parinode;
        ier->index = index;
        ier->n = utf8->Length;
        RtlCopyMemory(ier->name, utf8->Buffer, utf8->Length);
    
        if (!insert_tree_item(Vcb, subvol, searchkey.obj_id, searchkey.obj_type, searchkey.offset, ier, sizeof(INODE_EXTREF) - 1 + utf8->Length, NULL, rollback)) {
            ERR("error - failed to insert item\n");
            return STATUS_INTERNAL_ERROR;
        }
    }
    
    return STATUS_SUCCESS;
}

NTSTATUS add_inode_ref(device_extension* Vcb, root* subvol, UINT64 inode, UINT64 parinode, UINT64 index, PANSI_STRING utf8, LIST_ENTRY* rollback) {
    KEY searchkey;
    traverse_ptr tp;
    INODE_REF* ir;
    NTSTATUS Status;
    
    searchkey.obj_id = inode;
    searchkey.obj_type = TYPE_INODE_REF;
    searchkey.offset = parinode;
    
    Status = find_item(Vcb, subvol, &tp, &searchkey, FALSE);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08x\n", Status);
        return Status;
    }
    
    if (!keycmp(&searchkey, &tp.item->key)) {
        ULONG irsize = tp.item->size + sizeof(INODE_REF) - 1 + utf8->Length;
        UINT8* ir2;
        UINT32 maxlen = Vcb->superblock.node_size - sizeof(tree_header) - sizeof(leaf_node);
        
        if (irsize > maxlen) {
            if (Vcb->superblock.incompat_flags & BTRFS_INCOMPAT_FLAGS_EXTENDED_IREF) {
                TRACE("INODE_REF too long, creating INODE_EXTREF\n");
                return add_inode_extref(Vcb, subvol, inode, parinode, index, utf8, rollback);
            } else {
                ERR("item would be too long (%u > %u)\n", irsize, maxlen);
                return STATUS_INTERNAL_ERROR;
            }
        }
        
        ir2 = ExAllocatePoolWithTag(PagedPool, irsize, ALLOC_TAG);
        if (!ir2) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        if (tp.item->size > 0)
            RtlCopyMemory(ir2, tp.item->data, tp.item->size);
        
        ir = (INODE_REF*)&ir2[tp.item->size];
        ir->index = index;
        ir->n = utf8->Length;
        RtlCopyMemory(ir->name, utf8->Buffer, utf8->Length);
        
        delete_tree_item(Vcb, &tp, rollback);
        
        if (!insert_tree_item(Vcb, subvol, searchkey.obj_id, searchkey.obj_type, searchkey.offset, ir2, irsize, NULL, rollback)) {
            ERR("error - failed to insert item\n");
            return STATUS_INTERNAL_ERROR;
        }
    } else {
        ir = ExAllocatePoolWithTag(PagedPool, sizeof(INODE_REF) - 1 + utf8->Length, ALLOC_TAG);
        if (!ir) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ir->index = index;
        ir->n = utf8->Length;
        RtlCopyMemory(ir->name, utf8->Buffer, utf8->Length);
    
        if (!insert_tree_item(Vcb, subvol, searchkey.obj_id, searchkey.obj_type, searchkey.offset, ir, sizeof(INODE_REF) - 1 + ir->n, NULL, rollback)) {
            ERR("error - failed to insert item\n");
            return STATUS_INTERNAL_ERROR;
        }
    }
    
    return STATUS_SUCCESS;
}

// static NTSTATUS get_fileref_from_dir_item(device_extension* Vcb, file_ref** pfr, file_ref* parent, root* subvol, DIR_ITEM* di) {
//     LIST_ENTRY* le;
//     file_ref* fileref;
//     fcb* sf2;
//     KEY searchkey;
//     traverse_ptr tp;
//     NTSTATUS Status;
//     
//     le = parent->children.Flink;
//     
//     while (le != &parent->children) {
//         file_ref* c = CONTAINING_RECORD(le, file_ref, list_entry);
//         
//         if (c->fcb->inode == di->key.obj_id && c->fcb->subvol == subvol) {
// #ifdef DEBUG_FCB_REFCOUNTS
//             LONG rc = InterlockedIncrement(&c->refcount);
//             WARN("fileref %p: refcount now %i (%S)\n", c, rc, file_desc_fileref(c));
// #else
//             InterlockedIncrement(&c->refcount);
// #endif
//             *pfr = c;
//             return STATUS_SUCCESS;
//         }
//         
//         le = le->Flink;
//     }
//     
//     fileref = create_fileref();
//     if (!fileref) {
//         ERR("out of memory\n");
//         return STATUS_INSUFFICIENT_RESOURCES;
//     }
//     
//     sf2 = create_fcb();
//     if (!sf2) {
//         ERR("out of memory\n");
//         free_fileref(fileref);
//         return STATUS_INSUFFICIENT_RESOURCES;
//     }
//     
//     fileref->fcb = sf2;
//     sf2->Vcb = Vcb;
// 
//     fileref->utf8.Length = fileref->utf8.MaximumLength = di->n;
//     fileref->utf8.Buffer = ExAllocatePoolWithTag(PagedPool, di->n, ALLOC_TAG);
//     if (!fileref->utf8.Buffer) {
//         ERR("out of memory\n");
//         free_fileref(fileref);
//         return STATUS_INSUFFICIENT_RESOURCES;
//     }
//     
//     RtlCopyMemory(fileref->utf8.Buffer, di->name, di->n);
// 
//     parent->refcount++;
// #ifdef DEBUG_FCB_REFCOUNTS
//     WARN("fileref %p: refcount now %i (%S)\n", parent, parent->refcount, file_desc_fileref(parent));
// #endif
//     
//     if (di->key.obj_type == TYPE_ROOT_ITEM) {
//         root* fcbroot = NULL;
//         
//         le = Vcb->roots.Flink;
//         while (le != &Vcb->roots) {
//             root* r = CONTAINING_RECORD(le, root, list_entry);
//             
//             if (r->id == di->key.obj_id) {
//                 fcbroot = r;
//                 break;
//             }
//             
//             le = le->Flink;
//         }
//         
//         sf2->subvol = fcbroot;
//         sf2->inode = SUBVOL_ROOT_INODE;
//     } else {
//         sf2->subvol = subvol;
//         sf2->inode = di->key.obj_id;
//     }
//     
//     sf2->type = di->type;
//     
//     fileref->name_offset = parent->full_filename.Length / sizeof(WCHAR);
//    
//     if (parent != Vcb->root_fileref)
//         fileref->name_offset++;
//     
//     InsertTailList(&parent->children, &fileref->list_entry);
//     
//     searchkey.obj_id = sf2->inode;
//     searchkey.obj_type = TYPE_INODE_ITEM;
//     searchkey.offset = 0xffffffffffffffff;
//     
//     Status = find_item(Vcb, sf2->subvol, &tp, &searchkey, FALSE);
//     if (!NT_SUCCESS(Status)) {
//         ERR("error - find_item returned %08x\n", Status);
//         free_fileref(fileref);
//         return Status;
//     }
//     
//     if (tp.item->key.obj_id != searchkey.obj_id || tp.item->key.obj_type != searchkey.obj_type) {
//         ERR("couldn't find INODE_ITEM for inode %llx in subvol %llx\n", sf2->inode, sf2->subvol->id);
//         free_fileref(fileref);
//         return STATUS_INTERNAL_ERROR;
//     }
//     
//     if (tp.item->size > 0)
//         RtlCopyMemory(&sf2->inode_item, tp.item->data, min(sizeof(INODE_ITEM), tp.item->size));
//     
//     // This is just a quick function for the sake of move_across_subvols. As such, we don't bother
//     // with sf2->atts, sf2->sd, or sf2->full_filename.
//     
//     fileref->parent = (struct _file_ref*)parent;
//     InsertTailList(&parent->children, &fileref->list_entry);
//     
//     *pfr = fileref;
// 
//     return STATUS_SUCCESS;
// }

// static LONG get_tree_count(device_extension* Vcb, LIST_ENTRY* tc) {
//     LONG rc = 0;
//     LIST_ENTRY* le = Vcb->trees.Flink;
//     
//     while (le != &Vcb->trees) {
//         tree* t = CONTAINING_RECORD(le, tree, list_entry);
//         
//         rc += t->refcount;
//         
//         le = le->Flink;
//     }
//     
//     le = tc->Flink;
//     while (le != tc) {
//         tree_cache* tc2 = CONTAINING_RECORD(le, tree_cache, list_entry);
//         tree* t;
//         
//         rc--;
//         
//         t = tc2->tree->parent;
//         while (t) {
//             rc--;
//             t = t->parent;
//         }
//         
//         le = le->Flink;
//     }
//     
//     return rc;
// }

// static NTSTATUS STDCALL move_inode_across_subvols(device_extension* Vcb, file_ref* fileref, root* destsubvol, UINT64 destinode, UINT64 inode,
//                                                   UINT64 oldparinode, PANSI_STRING utf8, UINT32 crc32, BTRFS_TIME* now, LIST_ENTRY* rollback) {
//     UINT64 index;
//     UINT32 oldcrc32;
//     INODE_ITEM* ii;
//     BOOL has_hardlink = FALSE;
//     DIR_ITEM* di;
//     KEY searchkey;
//     traverse_ptr tp, next_tp;
//     NTSTATUS Status;
//     BOOL b;
//     
//     // move INODE_ITEM
//     
//     fileref->fcb->inode_item.transid = Vcb->superblock.generation;
//     fileref->fcb->inode_item.sequence++;
//     fileref->fcb->inode_item.st_ctime = *now;    
//     
//     searchkey.obj_id = fileref->fcb->inode;
//     searchkey.obj_type = TYPE_INODE_ITEM;
//     searchkey.offset = 0;
//     
//     Status = find_item(Vcb, fileref->fcb->subvol, &tp, &searchkey, FALSE);
//     if (!NT_SUCCESS(Status)) {
//         ERR("error - find_item returned %08x\n", Status);
//         return Status;
//     }
//     
//     if (!keycmp(&searchkey, &tp.item->key)) {
//         delete_tree_item(Vcb, &tp, rollback);
//         
//         if (fileref->fcb->inode_item.st_nlink > 1) {
//             fileref->fcb->inode_item.st_nlink--;
//             
//             ii = ExAllocatePoolWithTag(PagedPool, sizeof(INODE_ITEM), ALLOC_TAG);
//             if (!ii) {
//                 ERR("out of memory\n");
//                 return STATUS_INSUFFICIENT_RESOURCES;
//             }
//             
//             RtlCopyMemory(ii, &fileref->fcb->inode_item, sizeof(INODE_ITEM));
//             
//             if (!insert_tree_item(Vcb, fileref->fcb->subvol, fileref->fcb->inode, TYPE_INODE_ITEM, 0, ii, sizeof(INODE_ITEM), NULL, rollback)) {
//                 ERR("error - failed to insert item\n");
//                 return STATUS_INTERNAL_ERROR;
//             }
//             
//             has_hardlink = TRUE;
//         }
//     } else {
//         WARN("couldn't find old INODE_ITEM\n");
//     }
//     
//     fileref->fcb->inode_item.st_nlink = 1;
//     
//     ii = ExAllocatePoolWithTag(PagedPool, sizeof(INODE_ITEM), ALLOC_TAG);
//     if (!ii) {
//         ERR("out of memory\n");
//         return STATUS_INSUFFICIENT_RESOURCES;
//     }
//     
//     RtlCopyMemory(ii, &fileref->fcb->inode_item, sizeof(INODE_ITEM));
//     
//     if (!insert_tree_item(Vcb, destsubvol, inode, TYPE_INODE_ITEM, 0, ii, sizeof(INODE_ITEM), NULL, rollback)) {
//         ERR("error - failed to insert item\n");
//         return STATUS_INTERNAL_ERROR;
//     }
//     
//     oldcrc32 = calc_crc32c(0xfffffffe, (UINT8*)fileref->utf8.Buffer, (ULONG)fileref->utf8.Length);
//     
//     // delete old DIR_ITEM
//     
//     Status = delete_dir_item(Vcb, fileref->fcb->subvol, oldparinode, oldcrc32, &fileref->utf8, rollback);
//     if (!NT_SUCCESS(Status)) {
//         ERR("delete_dir_item returned %08x\n", Status);
//         return Status;
//     }
//     
//     // create new DIR_ITEM
//     
//     di = ExAllocatePoolWithTag(PagedPool, sizeof(DIR_ITEM) - 1 + utf8->Length, ALLOC_TAG);
//     if (!di) {
//         ERR("out of memory\n");
//         return STATUS_INSUFFICIENT_RESOURCES;
//     }
//                         
//     di->key.obj_id = inode;
//     di->key.obj_type = TYPE_INODE_ITEM;
//     di->key.offset = 0;
//     di->transid = Vcb->superblock.generation;
//     di->m = 0;
//     di->n = utf8->Length;
//     di->type = fileref->fcb->type;
//     RtlCopyMemory(di->name, utf8->Buffer, utf8->Length);
//     
//     Status = add_dir_item(Vcb, destsubvol, destinode, crc32, di, sizeof(DIR_ITEM) - 1 + utf8->Length, rollback);
//     if (!NT_SUCCESS(Status)) {
//         ERR("add_dir_item returned %08x\n", Status);
//         return Status;
//     }
//     
//     Status = delete_inode_ref(Vcb, fileref->fcb->subvol, fileref->fcb->inode, oldparinode, &fileref->utf8, rollback);
//     if (!NT_SUCCESS(Status)) {
//         ERR("delete_inode_ref returned %08x\n", Status);
//         return Status;
//     }
//     
//     // delete DIR_INDEX
//     
//     if (fileref->index == 0) {
//         WARN("couldn't find old INODE_REF\n");
//     } else {    
//         searchkey.obj_id = oldparinode;
//         searchkey.obj_type = TYPE_DIR_INDEX;
//         searchkey.offset = fileref->index;
//         
//         Status = find_item(Vcb, fileref->fcb->subvol, &tp, &searchkey, FALSE);
//         if (!NT_SUCCESS(Status)) {
//             ERR("error - find_item returned %08x\n", Status);
//             return Status;
//         }
//         
//         if (!keycmp(&searchkey, &tp.item->key))
//             delete_tree_item(Vcb, &tp, rollback);
//         else
//             WARN("couldn't find old DIR_INDEX\n");
//     }
//     
//     // get new index
//     
//     searchkey.obj_id = destinode;
//     searchkey.obj_type = TYPE_DIR_INDEX + 1;
//     searchkey.offset = 0;
//         
//     Status = find_item(Vcb, destsubvol, &tp, &searchkey, FALSE);
//     if (!NT_SUCCESS(Status)) {
//         ERR("error - find_item returned %08x\n", Status);
//         return Status;
//     }
//         
//     if (!keycmp(&searchkey, &tp.item->key)) {
//         if (find_prev_item(Vcb, &tp, &next_tp, FALSE)) {
//             tp = next_tp;
//                 
//             TRACE("moving back to %llx,%x,%llx\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset);
//         }
//     }
// 
//     if (tp.item->key.obj_id == searchkey.obj_id && tp.item->key.obj_type == TYPE_DIR_INDEX) {
//         index = tp.item->key.offset + 1;
//     } else
//         index = 2;
//         
//     // create INODE_REF
//     
//     Status = add_inode_ref(Vcb, destsubvol, inode, destinode, index, utf8, rollback);
//     if (!NT_SUCCESS(Status)) {
//         ERR("add_inode_ref returned %08x\n", Status);
//         return Status;
//     }
//     
//     // create DIR_INDEX
//     
//     di = ExAllocatePoolWithTag(PagedPool, sizeof(DIR_ITEM) - 1 + utf8->Length, ALLOC_TAG);
//     if (!di) {
//         ERR("out of memory\n");
//         return STATUS_INSUFFICIENT_RESOURCES;
//     }
//                         
//     di->key.obj_id = inode;
//     di->key.obj_type = TYPE_INODE_ITEM;
//     di->key.offset = 0;
//     di->transid = Vcb->superblock.generation;
//     di->m = 0;
//     di->n = utf8->Length;
//     di->type = fileref->fcb->type;
//     RtlCopyMemory(di->name, utf8->Buffer, utf8->Length);
//     
//     if (!insert_tree_item(Vcb, destsubvol, destinode, TYPE_DIR_INDEX, index, di, sizeof(DIR_ITEM) - 1 + utf8->Length, NULL, rollback)) {
//         ERR("error - failed to insert item\n");
//         return STATUS_INTERNAL_ERROR;
//     }
//     
//     // move XATTR_ITEMs
//     
//     searchkey.obj_id = fileref->fcb->inode;
//     searchkey.obj_type = TYPE_XATTR_ITEM;
//     searchkey.offset = 0;
//     
//     Status = find_item(Vcb, fileref->fcb->subvol, &tp, &searchkey, FALSE);
//     if (!NT_SUCCESS(Status)) {
//         ERR("error - find_item returned %08x\n", Status);
//         return Status;
//     }
//     
//     do {
//         if (tp.item->key.obj_id == fileref->fcb->inode && tp.item->key.obj_type == TYPE_XATTR_ITEM && tp.item->size > 0) {
//             di = ExAllocatePoolWithTag(PagedPool, tp.item->size, ALLOC_TAG);
//             
//             if (!di) {
//                 ERR("out of memory\n");
//                 return STATUS_INSUFFICIENT_RESOURCES;
//             }
//             
//             RtlCopyMemory(di, tp.item->data, tp.item->size);
//             
//             if (!insert_tree_item(Vcb, destsubvol, inode, TYPE_XATTR_ITEM, tp.item->key.offset, di, tp.item->size, NULL, rollback)) {
//                 ERR("error - failed to insert item\n");
//                 return STATUS_INTERNAL_ERROR;
//             }
//             
//             if (!has_hardlink)
//                 delete_tree_item(Vcb, &tp, rollback);
//         }
//         
//         b = find_next_item(Vcb, &tp, &next_tp, FALSE);
//         if (b) {
//             tp = next_tp;
//             
//             if (next_tp.item->key.obj_id > fileref->fcb->inode || next_tp.item->key.obj_type > TYPE_XATTR_ITEM)
//                 break;
//         }
//     } while (b);
//     
//     // do extents
//     
//     searchkey.obj_id = fileref->fcb->inode;
//     searchkey.obj_type = TYPE_EXTENT_DATA;
//     searchkey.offset = 0;
//     
//     Status = find_item(Vcb, fileref->fcb->subvol, &tp, &searchkey, FALSE);
//     if (!NT_SUCCESS(Status)) {
//         ERR("error - find_item returned %08x\n", Status);
//         return Status;
//     }
//     
//     do {
//         if (tp.item->key.obj_id == fileref->fcb->inode && tp.item->key.obj_type == TYPE_EXTENT_DATA) {
//             if (tp.item->size < sizeof(EXTENT_DATA)) {
//                 ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(EXTENT_DATA));
//             } else {
//                 EXTENT_DATA* ed = ExAllocatePoolWithTag(PagedPool, tp.item->size, ALLOC_TAG);
//                 
//                 if (!ed) {
//                     ERR("out of memory\n");
//                     return STATUS_INSUFFICIENT_RESOURCES;
//                 }
//                 
//                 RtlCopyMemory(ed, tp.item->data, tp.item->size);
//                 
//                 // FIXME - update ed's generation
//                         
//                 if (!insert_tree_item(Vcb, destsubvol, inode, TYPE_EXTENT_DATA, tp.item->key.offset, ed, tp.item->size, NULL, rollback)) {
//                     ERR("error - failed to insert item\n");
//                     return STATUS_INTERNAL_ERROR;
//                 }
//             
//                 if ((ed->type == EXTENT_TYPE_REGULAR || ed->type == EXTENT_TYPE_PREALLOC) && tp.item->size >= sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2)) {
//                     EXTENT_DATA2* ed2 = (EXTENT_DATA2*)ed->data;
//                     
//                     if (ed2->address != 0) {
//                         Status = increase_extent_refcount_data(Vcb, ed2->address, ed2->size, destsubvol, inode, tp.item->key.offset - ed2->offset, 1, rollback);
//                         if (!NT_SUCCESS(Status)) {
//                             ERR("increase_extent_refcount_data returned %08x\n", Status);
//                             return Status;
//                         }
//                         
//                         if (!has_hardlink) {
//                             Status = decrease_extent_refcount_data(Vcb, ed2->address, ed2->size, fileref->fcb->subvol, fileref->fcb->inode,
//                                                                    tp.item->key.offset - ed2->offset, 1, NULL, rollback);
//                         
//                             if (!NT_SUCCESS(Status)) {
//                                 ERR("decrease_extent_refcount_data returned %08x\n", Status);
//                                 return Status;
//                             }
//                         }
//                     }
//                 }
//                 
//                 if (!has_hardlink)
//                     delete_tree_item(Vcb, &tp, rollback);
//             }
//         }
//         
//         b = find_next_item(Vcb, &tp, &next_tp, FALSE);
//         if (b) {
//             tp = next_tp;
//             
//             if (next_tp.item->key.obj_id > fileref->fcb->inode || next_tp.item->key.obj_type > TYPE_EXTENT_DATA)
//                 break;
//         }
//     } while (b);
//     
//     return STATUS_SUCCESS;
// }

// typedef struct {
//     file_ref* fileref;
//     UINT8 level;
//     UINT32 crc32;
//     UINT64 newinode;
//     UINT64 newparinode;
//     BOOL subvol;
//     ANSI_STRING utf8;
//     LIST_ENTRY list_entry;
// } dir_list;
// 
// static NTSTATUS add_to_dir_list(file_ref* fileref, UINT8 level, LIST_ENTRY* dl, UINT64 newparinode, BOOL* empty) {
//     KEY searchkey;
//     traverse_ptr tp, next_tp;
//     BOOL b;
//     NTSTATUS Status;
//     
//     *empty = TRUE;
//     
//     searchkey.obj_id = fileref->fcb->inode;
//     searchkey.obj_type = TYPE_DIR_INDEX;
//     searchkey.offset = 2;
//     
//     Status = find_item(fileref->fcb->Vcb, fileref->fcb->subvol, &tp, &searchkey, FALSE);
//     if (!NT_SUCCESS(Status)) {
//         ERR("error - find_item returned %08x\n", Status);
//         return Status;
//     }
//     
//     do {
//         if (tp.item->key.obj_id == fileref->fcb->inode && tp.item->key.obj_type == TYPE_DIR_INDEX) {
//             if (tp.item->size < sizeof(DIR_ITEM)) {
//                 ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(DIR_ITEM));
//             } else {
//                 DIR_ITEM* di = (DIR_ITEM*)tp.item->data;
//                 file_ref* child;
//                 dir_list* dl2;
//                 
//                 if (tp.item->size < sizeof(DIR_ITEM) - 1 + di->n + di->m) {
//                     ERR("(%llx,%x,%llx) was truncated\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset);
//                 } else {
//                     if (di->key.obj_type == TYPE_INODE_ITEM || di->key.obj_type == TYPE_ROOT_ITEM) {
//                         if (di->key.obj_type == TYPE_ROOT_ITEM)
//                             TRACE("moving subvol %llx\n", di->key.obj_id);
//                         else
//                             TRACE("moving inode %llx\n", di->key.obj_id);
//                         
//                         *empty = FALSE;
//                         
//                         Status = get_fileref_from_dir_item(fileref->fcb->Vcb, &child, fileref, fileref->fcb->subvol, di);
//                         if (!NT_SUCCESS(Status)) {
//                             ERR("get_fileref_from_dir_item returned %08x\n", Status);
//                             return Status;
//                         }
//                         
//                         dl2 = ExAllocatePoolWithTag(PagedPool, sizeof(dir_list), ALLOC_TAG);
//                         if (!dl2) {
//                             ERR("out of memory\n");
//                             return STATUS_INSUFFICIENT_RESOURCES;
//                         }
//                         
//                         dl2->fileref = child;
//                         dl2->level = level;
//                         dl2->newparinode = newparinode;
//                         dl2->subvol = di->key.obj_type == TYPE_ROOT_ITEM;
//                         
//                         dl2->utf8.Length = dl2->utf8.MaximumLength = di->n;
//                         dl2->utf8.Buffer = ExAllocatePoolWithTag(PagedPool, dl2->utf8.MaximumLength, ALLOC_TAG);
//                         if (!dl2->utf8.Buffer) {
//                             ERR("out of memory\n");
//                             return STATUS_INSUFFICIENT_RESOURCES;
//                         }
//                         
//                         RtlCopyMemory(dl2->utf8.Buffer, di->name, dl2->utf8.Length);
//                         dl2->crc32 = calc_crc32c(0xfffffffe, (UINT8*)dl2->utf8.Buffer, (ULONG)dl2->utf8.Length);
//                         
//                         InsertTailList(dl, &dl2->list_entry);
//                     }
//                 }
//             }
//         }
//         
//         b = find_next_item(fileref->fcb->Vcb, &tp, &next_tp, FALSE);
//         if (b) {
//             tp = next_tp;
//             
//             if (tp.item->key.obj_id > searchkey.obj_id || tp.item->key.obj_type > searchkey.obj_type)
//                 break;
//         }
//     } while (b);
//     
//     return STATUS_SUCCESS;
// }

// static NTSTATUS STDCALL move_across_subvols(device_extension* Vcb, file_ref* fileref, root* destsubvol, UINT64 destinode, PANSI_STRING utf8, UINT32 crc32, BTRFS_TIME* now, LIST_ENTRY* rollback) {
//     UINT64 inode, oldparinode;
//     NTSTATUS Status;
//     LIST_ENTRY dl;
//     
//     if (fileref->fcb->inode_item.st_nlink > 1 && fileref->fcb->open_count > 1) {
//         WARN("not moving hard-linked inode across subvols when open more than once\n");
//         // FIXME - don't do this if only one fileref?
//         return STATUS_ACCESS_DENIED;
//     }
//     
//     if (destsubvol->lastinode == 0)
//         get_last_inode(Vcb, destsubvol);
//     
//     inode = destsubvol->lastinode + 1;
//     destsubvol->lastinode++;
//     
//     oldparinode = fileref->fcb->subvol == fileref->parent->fcb->subvol ? fileref->parent->fcb->inode : SUBVOL_ROOT_INODE;
//     
//     Status = move_inode_across_subvols(Vcb, fileref, destsubvol, destinode, inode, oldparinode, utf8, crc32, now, rollback);
//     if (!NT_SUCCESS(Status)) {
//         ERR("move_inode_across_subvols returned %08x\n", Status);
//         return Status;
//     }
//     
//     if (fileref->fcb->type == BTRFS_TYPE_DIRECTORY && fileref->fcb->inode_item.st_size > 0) {
//         BOOL b, empty;
//         UINT8 level, max_level;
//         LIST_ENTRY* le;
//         
//         InitializeListHead(&dl);
//         
//         add_to_dir_list(fileref, 0, &dl, inode, &b);
//         
//         level = 0;
//         do {
//             empty = TRUE;
//             
//             le = dl.Flink;
//             while (le != &dl) {
//                 dir_list* dl2 = CONTAINING_RECORD(le, dir_list, list_entry);
//                 
//                 if (dl2->level == level && !dl2->subvol) {
//                     inode++;
//                     destsubvol->lastinode++;
//                     
//                     dl2->newinode = inode;
//                     
//                     if (dl2->fileref->fcb->type == BTRFS_TYPE_DIRECTORY) {
//                         add_to_dir_list(dl2->fileref, level+1, &dl, dl2->newinode, &b);
//                         if (!b) empty = FALSE;
//                     }
//                 }
//                 
//                 le = le->Flink;
//             }
//             
//             if (!empty) level++;
//         } while (!empty);
//         
//         max_level = level;
//         
//         for (level = 0; level <= max_level; level++) {
//             TRACE("level %u\n", level);
//             
//             le = dl.Flink;
//             while (le != &dl) {
//                 dir_list* dl2 = CONTAINING_RECORD(le, dir_list, list_entry);
//                 
//                 if (dl2->level == level) {
//                     if (dl2->subvol) {
//                         TRACE("subvol %llx\n", dl2->fileref->fcb->subvol->id);
//                         
//                         Status = move_subvol(Vcb, dl2->fileref, destsubvol, dl2->newparinode, &dl2->utf8, dl2->crc32, dl2->crc32, now, FALSE, rollback);
//                         if (!NT_SUCCESS(Status)) {
//                             ERR("move_subvol returned %08x\n", Status);
//                             return Status;
//                         }
//                     } else {
//                         TRACE("inode %llx\n", dl2->fileref->fcb->inode);
// 
//                         Status = move_inode_across_subvols(Vcb, dl2->fileref, destsubvol, dl2->newparinode, dl2->newinode, dl2->fileref->parent->fcb->inode, &dl2->utf8, dl2->crc32, now, rollback);
//                         if (!NT_SUCCESS(Status)) {
//                             ERR("move_inode_across_subvols returned %08x\n", Status);
//                             return Status;
//                         }
//                     }
//                 }
//                 
//                 le = le->Flink;
//             }
//         }
//         
//         while (!IsListEmpty(&dl)) {
//             dir_list* dl2;
//             
//             le = RemoveHeadList(&dl);
//             dl2 = CONTAINING_RECORD(le, dir_list, list_entry);
//             
//             ExFreePool(dl2->utf8.Buffer);
//             free_fileref(dl2->fileref);
//             
//             ExFreePool(dl2);
//         }
//     }
//     
//     fileref->fcb->inode = inode;
//     fileref->fcb->subvol = destsubvol;
//       
//     fileref->fcb->subvol->root_item.ctransid = Vcb->superblock.generation;
//     fileref->fcb->subvol->root_item.ctime = *now;
//     
//     RemoveEntryList(&fileref->fcb->list_entry);
//     InsertTailList(&fileref->fcb->subvol->fcbs, &fileref->fcb->list_entry);
//     
//     if (fileref->fcb->debug_desc) {
//         ExFreePool(fileref->fcb->debug_desc);
//         fileref->fcb->debug_desc = NULL;
//     }
//     
//     return STATUS_SUCCESS;
// }

// static NTSTATUS STDCALL move_subvol(device_extension* Vcb, file_ref* fileref, root* destsubvol, UINT64 destinode, PANSI_STRING utf8, UINT32 crc32,
//                                     UINT32 oldcrc32, BTRFS_TIME* now, BOOL ReplaceIfExists, LIST_ENTRY* rollback) {
//     DIR_ITEM* di;
//     NTSTATUS Status;
//     KEY searchkey;
//     traverse_ptr tp;
//     UINT64 oldindex, index;
//     ROOT_REF* rr;
//     
//     // delete old DIR_ITEM
//     
//     Status = delete_dir_item(Vcb, fileref->parent->fcb->subvol, fileref->parent->fcb->inode, oldcrc32, &fileref->utf8, rollback);
//     if (!NT_SUCCESS(Status)) {
//         ERR("delete_dir_item returned %08x\n", Status);
//         return Status;
//     }
//     
//     // create new DIR_ITEM
//     
//     di = ExAllocatePoolWithTag(PagedPool, sizeof(DIR_ITEM) - 1 + utf8->Length, ALLOC_TAG);
//     if (!di) {
//         ERR("out of memory\n");
//         return STATUS_INSUFFICIENT_RESOURCES;
//     }
//         
//     di->key.obj_id = fileref->fcb->subvol->id;
//     di->key.obj_type = TYPE_ROOT_ITEM;
//     di->key.offset = 0;
//     di->transid = Vcb->superblock.generation;
//     di->m = 0;
//     di->n = utf8->Length;
//     di->type = fileref->fcb->type;
//     RtlCopyMemory(di->name, utf8->Buffer, utf8->Length);
//     
//     Status = add_dir_item(Vcb, destsubvol, destinode, crc32, di, sizeof(DIR_ITEM) - 1 + utf8->Length, rollback);
//     if (!NT_SUCCESS(Status)) {
//         ERR("add_dir_item returned %08x\n", Status);
//         return Status;
//     }
//     
//     // delete old ROOT_REF
//     
//     oldindex = 0;
//     
//     Status = delete_root_ref(Vcb, fileref->fcb->subvol->id, fileref->parent->fcb->subvol->id, fileref->parent->fcb->inode, &fileref->utf8, &oldindex, rollback);
//     if (!NT_SUCCESS(Status)) {
//         ERR("delete_root_ref returned %08x\n", Status);
//         return Status;
//     }
//     
//     TRACE("root index = %llx\n", oldindex);
//     
//     // delete old DIR_INDEX
//     
//     if (oldindex != 0) {
//         searchkey.obj_id = fileref->parent->fcb->inode;
//         searchkey.obj_type = TYPE_DIR_INDEX;
//         searchkey.offset = oldindex;
//         
//         Status = find_item(Vcb, fileref->parent->fcb->subvol, &tp, &searchkey, FALSE);
//         if (!NT_SUCCESS(Status)) {
//             ERR("error - find_item returned %08x\n", Status);
//             return Status;
//         }
//         
//         if (!keycmp(&searchkey, &tp.item->key)) {
//             TRACE("deleting (%llx,%x,%llx)\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset);
//             
//             delete_tree_item(Vcb, &tp, rollback);
//         } else {
//             WARN("could not find old DIR_INDEX entry\n");
//         }
//     }
//     
//     // create new DIR_INDEX
//     
//     if (fileref->parent->fcb->subvol == destsubvol && fileref->parent->fcb->inode == destinode) {
//         index = oldindex;
//     } else {
//         index = find_next_dir_index(Vcb, destsubvol, destinode);
//     }
//     
//     di = ExAllocatePoolWithTag(PagedPool, sizeof(DIR_ITEM) - 1 + utf8->Length, ALLOC_TAG);
//     if (!di) {
//         ERR("out of memory\n");
//         return STATUS_INSUFFICIENT_RESOURCES;
//     }
//         
//     di->key.obj_id = fileref->fcb->subvol->id;
//     di->key.obj_type = TYPE_ROOT_ITEM;
//     di->key.offset = 0;
//     di->transid = Vcb->superblock.generation;
//     di->m = 0;
//     di->n = utf8->Length;
//     di->type = fileref->fcb->type;
//     RtlCopyMemory(di->name, utf8->Buffer, utf8->Length);
//     
//     if (!insert_tree_item(Vcb, destsubvol, destinode, TYPE_DIR_INDEX, index, di, sizeof(DIR_ITEM) - 1 + utf8->Length, NULL, rollback)) {
//         ERR("error - failed to insert item\n");
//         return STATUS_INTERNAL_ERROR;
//     }
//     
//     // create new ROOT_REF
//     
//     rr = ExAllocatePoolWithTag(PagedPool, sizeof(ROOT_REF) - 1 + utf8->Length, ALLOC_TAG);
//     if (!rr) {
//         ERR("out of memory\n");
//         return STATUS_INSUFFICIENT_RESOURCES;
//     }
//     
//     rr->dir = destinode;
//     rr->index = index;
//     rr->n = utf8->Length;
//     RtlCopyMemory(rr->name, utf8->Buffer, utf8->Length);
//     
//     Status = add_root_ref(Vcb, fileref->fcb->subvol->id, destsubvol->id, rr, rollback);
//     if (!NT_SUCCESS(Status)) {
//         ERR("add_root_ref returned %08x\n", Status);
//         return Status;
//     }
//     
//     Status = update_root_backref(Vcb, fileref->fcb->subvol->id, fileref->parent->fcb->subvol->id, rollback);
//     if (!NT_SUCCESS(Status)) {
//         ERR("update_root_backref 1 returned %08x\n", Status);
//         return Status;
//     }
//     
//     if (fileref->parent->fcb->subvol != destsubvol) {
//         Status = update_root_backref(Vcb, fileref->fcb->subvol->id, destsubvol->id, rollback);
//         if (!NT_SUCCESS(Status)) {
//             ERR("update_root_backref 1 returned %08x\n", Status);
//             return Status;
//         }
//         
//         fileref->parent->fcb->subvol->root_item.ctransid = Vcb->superblock.generation;
//         fileref->parent->fcb->subvol->root_item.ctime = *now;
//     }
//     
//     destsubvol->root_item.ctransid = Vcb->superblock.generation;
//     destsubvol->root_item.ctime = *now;
//     
//     return STATUS_SUCCESS;
// }

BOOL has_open_children(file_ref* fileref) {
    LIST_ENTRY* le = fileref->children.Flink;
    
    if (IsListEmpty(&fileref->children))
        return FALSE;
        
    while (le != &fileref->children) {
        file_ref* c = CONTAINING_RECORD(le, file_ref, list_entry);
        
        if (c->fcb->open_count > 0)
            return TRUE;
        
        if (has_open_children(c))
            return TRUE;

        le = le->Flink;
    }
    
    return FALSE;
}

// static NTSTATUS STDCALL set_rename_information(device_extension* Vcb, PIRP Irp, PFILE_OBJECT FileObject, PFILE_OBJECT tfo, BOOL ReplaceIfExists) {
//     FILE_RENAME_INFORMATION* fri = Irp->AssociatedIrp.SystemBuffer;
//     fcb *fcb = FileObject->FsContext, *tfofcb/*, *oldfcb*/;
//     file_ref *fileref, *oldfileref = NULL, *related;
//     ccb* ccb = FileObject->FsContext2;
//     root* parsubvol;
//     UINT64 parinode, dirpos;
//     WCHAR* fn;
//     UNICODE_STRING fnus;
//     ULONG fnlen, utf8len, disize;
//     NTSTATUS Status;
//     ANSI_STRING utf8;
//     UINT32 crc32, oldcrc32;
//     KEY searchkey;
//     traverse_ptr tp, next_tp;
//     DIR_ITEM* di;
//     LARGE_INTEGER time;
//     BTRFS_TIME now;
//     BOOL across_directories;
//     LONG i;
//     LIST_ENTRY rollback;
//     
//     InitializeListHead(&rollback);
//     
//     // FIXME - MSDN says we should be able to rename streams here, but I can't get it to work.
//     
//     // FIXME - don't ignore fri->RootDirectory
//     TRACE("    tfo = %p\n", tfo);
//     TRACE("    ReplaceIfExists = %u\n", ReplaceIfExists);
//     TRACE("    RootDirectory = %p\n", fri->RootDirectory);
//     TRACE("    FileName = %.*S\n", fri->FileNameLength / sizeof(WCHAR), fri->FileName);
//     
//     ExAcquireResourceExclusiveLite(&Vcb->tree_lock, TRUE);
//     
//     if (!ccb->fileref) {
//         ERR("tried to rename file with no fileref\n");
//         Status = STATUS_INVALID_PARAMETER;
//         goto end;
//     }
//     
//     fileref = ccb->fileref;
//     
//     KeQuerySystemTime(&time);
//     win_time_to_unix(time, &now);
//     
//     utf8.Buffer = NULL;
//     
//     if (!fileref->parent) {
//         ERR("error - tried to rename file with no parent\n");
//         Status = STATUS_ACCESS_DENIED;
//         goto end;
//     }
//     
//     fn = fri->FileName;
//     fnlen = fri->FileNameLength / sizeof(WCHAR);
//     
//     if (!tfo) {
//         parsubvol = fileref->parent->fcb->subvol;
//         parinode = fileref->parent->fcb->inode;
//         tfofcb = NULL;
//         
//         across_directories = FALSE;
//     } else {
//         tfofcb = tfo->FsContext;
//         parsubvol = tfofcb->subvol;
//         parinode = tfofcb->inode;
//         
//         for (i = fnlen - 1; i >= 0; i--) {
//             if (fri->FileName[i] == '\\' || fri->FileName[i] == '/') {
//                 fn = &fri->FileName[i+1];
//                 fnlen = (fri->FileNameLength / sizeof(WCHAR)) - i - 1;
//                 break;
//             }
//         }
//         
//         across_directories = parsubvol != fileref->parent->fcb->subvol || parinode != fileref->parent->fcb->inode;
//     }
//     
//     fnus.Buffer = fn;
//     fnus.Length = fnus.MaximumLength = fnlen * sizeof(WCHAR);
//     
//     TRACE("fnus = %.*S\n", fnus.Length / sizeof(WCHAR), fnus.Buffer);
//     
//     if (!is_file_name_valid(&fnus)) {
//         Status = STATUS_OBJECT_NAME_INVALID;
//         goto end;
//     }
//     
//     Status = RtlUnicodeToUTF8N(NULL, 0, &utf8len, fn, (ULONG)fnlen * sizeof(WCHAR));
//     if (!NT_SUCCESS(Status))
//         goto end;
//     
//     utf8.MaximumLength = utf8.Length = utf8len;
//     utf8.Buffer = ExAllocatePoolWithTag(PagedPool, utf8.MaximumLength, ALLOC_TAG);
//     if (!utf8.Buffer) {
//         ERR("out of memory\n");
//         Status = STATUS_INSUFFICIENT_RESOURCES;
//         goto end;
//     }
//     
//     Status = RtlUnicodeToUTF8N(utf8.Buffer, utf8len, &utf8len, fn, (ULONG)fnlen * sizeof(WCHAR));
//     if (!NT_SUCCESS(Status))
//         goto end;
//     
//     crc32 = calc_crc32c(0xfffffffe, (UINT8*)utf8.Buffer, (ULONG)utf8.Length);
//     
//     // FIXME - set to crc32 if utf8 and oldutf8 are identical
//     oldcrc32 = calc_crc32c(0xfffffffe, (UINT8*)fileref->utf8.Buffer, (ULONG)fileref->utf8.Length);
//     
// //     TRACE("utf8 fn = %s (%08x), old utf8 fn = %s (%08x)\n", utf8, crc32, oldutf8, oldcrc32);
//     
//     if (tfo && tfo->FsContext2) {
//         struct _ccb* relatedccb = tfo->FsContext2;
//         
//         related = relatedccb->fileref;
//     } else
//         related = NULL;
// 
// //     Status = get_fcb(Vcb, &oldfcb, &fnus, tfo ? tfo->FsContext : NULL, FALSE, NULL);
//     Status = open_fileref(Vcb, &oldfileref, &fnus, related, FALSE, NULL);
// 
//     if (NT_SUCCESS(Status)) {
//         WARN("destination file %S already exists\n", file_desc_fileref(oldfileref));
//         
//         if (fileref != oldfileref && !(oldfileref->fcb->open_count == 0 && oldfileref->deleted)) {
//             if (!ReplaceIfExists) {
//                 Status = STATUS_OBJECT_NAME_COLLISION;
//                 goto end;
//             } else if (oldfileref->fcb->open_count >= 1 && !oldfileref->deleted) {
//                 WARN("trying to overwrite open file\n");
//                 Status = STATUS_ACCESS_DENIED;
//                 goto end;
//             }
//             
//             if (oldfileref->fcb->type == BTRFS_TYPE_DIRECTORY) {
//                 WARN("trying to overwrite directory\n");
//                 Status = STATUS_ACCESS_DENIED;
//                 goto end;
//             }
//         }
//     }
//     
//     if (has_open_children(fileref)) {
//         WARN("trying to rename file with open children\n");
//         Status = STATUS_ACCESS_DENIED;
//         goto end;
//     }
//     
//     if (oldfileref) {
//         // FIXME - check we have permission to delete oldfileref
//         Status = delete_fileref(oldfileref, NULL, &rollback);
//         if (!NT_SUCCESS(Status)) {
//             ERR("delete_fileref returned %08x\n", Status);
//             goto end;
//         }
//     }
//     
//     if (fcb->inode == SUBVOL_ROOT_INODE) {
//         Status = move_subvol(Vcb, fileref, tfofcb->subvol, tfofcb->inode, &utf8, crc32, oldcrc32, &now, ReplaceIfExists, &rollback);
//         
//         if (!NT_SUCCESS(Status)) {
//             ERR("move_subvol returned %08x\n", Status);
//             goto end;
//         }
//     } else if (parsubvol != fcb->subvol) {
//         Status = move_across_subvols(Vcb, fileref, tfofcb->subvol, tfofcb->inode, &utf8, crc32, &now, &rollback);
//         
//         if (!NT_SUCCESS(Status)) {
//             ERR("move_across_subvols returned %08x\n", Status);
//             goto end;
//         }
//     } else {
//         // delete old DIR_ITEM entry
//         
//         Status = delete_dir_item(Vcb, fcb->subvol, fileref->parent->fcb->inode, oldcrc32, &fileref->utf8, &rollback);
//         if (!NT_SUCCESS(Status)) {
//             ERR("delete_dir_item returned %08x\n", Status);
//             goto end;
//         }
//         
//         // FIXME - make sure fcb's filepart matches the case on disk
//         
//         // create new DIR_ITEM entry
//         
//         di = ExAllocatePoolWithTag(PagedPool, sizeof(DIR_ITEM) - 1 + utf8.Length, ALLOC_TAG);
//         if (!di) {
//             ERR("out of memory\n");
//             Status = STATUS_INSUFFICIENT_RESOURCES;
//             goto end;
//         }
//         
//         di->key.obj_id = fcb->inode;
//         di->key.obj_type = TYPE_INODE_ITEM;
//         di->key.offset = 0;
//         di->transid = Vcb->superblock.generation;
//         di->m = 0;
//         di->n = utf8.Length;
//         di->type = fcb->type;
//         RtlCopyMemory(di->name, utf8.Buffer, utf8.Length);
//         
//         Status = add_dir_item(Vcb, parsubvol, parinode, crc32, di, sizeof(DIR_ITEM) - 1 + utf8.Length, &rollback);
//         if (!NT_SUCCESS(Status)) {
//             ERR("add_dir_item returned %08x\n", Status);
//             goto end;
//         }
//         
//         Status = delete_inode_ref(Vcb, fcb->subvol, fcb->inode, fileref->parent->fcb->inode, &fileref->utf8, &rollback);
//         if (!NT_SUCCESS(Status)) {
//             ERR("delete_inode_ref returned %08x\n", Status);
//             goto end;
//         }
// 
//         // delete old DIR_INDEX entry
//         
//         if (fileref->index != 0) {
//             searchkey.obj_id = fileref->parent->fcb->inode;
//             searchkey.obj_type = TYPE_DIR_INDEX;
//             searchkey.offset = fileref->index;
//             
//             Status = find_item(Vcb, fileref->parent->fcb->subvol, &tp, &searchkey, FALSE);
//             if (!NT_SUCCESS(Status)) {
//                 ERR("error - find_item returned %08x\n", Status);
//                 goto end;
//             }
//             
//             if (!keycmp(&tp.item->key, &searchkey))
//                 delete_tree_item(Vcb, &tp, &rollback);
//             else {
//                 WARN("couldn't find DIR_INDEX\n");
//             }
//         } else {
//             WARN("couldn't get index from INODE_REF\n");
//         }
//         
//         // create new DIR_INDEX entry
//         
//         if (parsubvol != fileref->parent->fcb->subvol || parinode != fileref->parent->fcb->inode) {
//             searchkey.obj_id = parinode;
//             searchkey.obj_type = TYPE_DIR_INDEX + 1;
//             searchkey.offset = 0;
//             
//             Status = find_item(Vcb, parsubvol, &tp, &searchkey, FALSE);
//             if (!NT_SUCCESS(Status)) {
//                 ERR("error - find_item returned %08x\n", Status);
//                 goto end;
//             }
//             
//             dirpos = 2;
//             
//             do {
//                 TRACE("%llx,%x,%llx\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset);
//                 
//                 if (tp.item->key.obj_id == searchkey.obj_id && tp.item->key.obj_type == TYPE_DIR_INDEX) {
//                     dirpos = tp.item->key.offset + 1;
//                     break;
//                 }
//                 
//                 if (find_prev_item(Vcb, &tp, &next_tp, FALSE)) {
//                     tp = next_tp;
//                 } else
//                     break;
//             } while (tp.item->key.obj_id >= parinode && tp.item->key.obj_type >= TYPE_DIR_INDEX);
//         } else
//             dirpos = fileref->index;
//         
//         disize = (ULONG)(sizeof(DIR_ITEM) - 1 + utf8.Length);
//         di = ExAllocatePoolWithTag(PagedPool, disize, ALLOC_TAG);
//         if (!di) {
//             ERR("out of memory\n");
//             Status = STATUS_INSUFFICIENT_RESOURCES;
//             goto end;
//         }
//         
//         di->key.obj_id = fcb->inode;
//         di->key.obj_type = TYPE_INODE_ITEM;
//         di->key.offset = 0;
//         di->transid = Vcb->superblock.generation;
//         di->m = 0;
//         di->n = (UINT16)utf8.Length;
//         di->type = fcb->type;
//         RtlCopyMemory(di->name, utf8.Buffer, utf8.Length);
//         
//         if (!insert_tree_item(Vcb, parsubvol, parinode, TYPE_DIR_INDEX, dirpos, di, disize, NULL, &rollback))
//             ERR("error - failed to insert item\n");
//         
//         // create new INODE_REF entry
//         
//         Status = add_inode_ref(Vcb, parsubvol, fcb->inode, parinode, dirpos, &utf8, &rollback);
//         if (!NT_SUCCESS(Status)) {
//             ERR("add_inode_ref returned %08x\n", Status);
//             goto end;
//         }
//         
//         fcb->inode_item.transid = Vcb->superblock.generation;
//         fcb->inode_item.sequence++;
//         fcb->inode_item.st_ctime = now;
//         
//         mark_fcb_dirty(fcb);
//     }
//     
//     // update directory INODE_ITEMs
//     
//     fileref->parent->fcb->inode_item.transid = Vcb->superblock.generation;
//     fileref->parent->fcb->inode_item.sequence++;
//     fileref->parent->fcb->inode_item.st_ctime = now;
//     fileref->parent->fcb->inode_item.st_mtime = now;
//     
//     TRACE("fileref->parent->fcb->inode_item.st_size was %llx\n", fileref->parent->fcb->inode_item.st_size);
//     if (!tfofcb || (fileref->parent->fcb->inode == tfofcb->inode && fileref->parent->fcb->subvol == tfofcb->subvol)) {
//         fileref->parent->fcb->inode_item.st_size += 2 * (utf8.Length - fileref->utf8.Length);
//     } else {
//         fileref->parent->fcb->inode_item.st_size -= 2 * fileref->utf8.Length;
//         TRACE("tfofcb->inode_item.st_size was %llx\n", tfofcb->inode_item.st_size);
//         tfofcb->inode_item.st_size += 2 * utf8.Length;
//         TRACE("tfofcb->inode_item.st_size now %llx\n", tfofcb->inode_item.st_size);
//         tfofcb->inode_item.transid = Vcb->superblock.generation;
//         tfofcb->inode_item.sequence++;
//         tfofcb->inode_item.st_ctime = now;
//         tfofcb->inode_item.st_mtime = now;
//     }
//     TRACE("fileref->parent->fcb->inode_item.st_size now %llx\n", fileref->parent->fcb->inode_item.st_size);
//     
//     if (oldfileref && oldfileref->fcb && oldfileref->parent->fcb != fileref->parent->fcb) {
//         TRACE("oldfileref->parent->fcb->inode_item.st_size was %llx\n", oldfileref->parent->fcb->inode_item.st_size);
//         oldfileref->parent->fcb->inode_item.st_size -= 2 * oldfileref->utf8.Length;
//         TRACE("oldfileref->parent->fcb->inode_item.st_size now %llx\n", oldfileref->parent->fcb->inode_item.st_size);
//     }
//     
//     mark_fcb_dirty(fileref->parent->fcb);
//     
//     if (tfofcb && (fileref->parent->fcb->inode != tfofcb->inode || fileref->parent->fcb->subvol != tfofcb->subvol))
//         mark_fcb_dirty(tfofcb);
//     
//     fcb->subvol->root_item.ctransid = Vcb->superblock.generation;
//     fcb->subvol->root_item.ctime = now;
//     
//     // FIXME - handle overwrite by rename here
//     send_notification_fileref(fileref, fcb->type == BTRFS_TYPE_DIRECTORY ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
//                               across_directories ? FILE_ACTION_REMOVED : FILE_ACTION_RENAMED_OLD_NAME);
// 
//     // FIXME - change full_filename and name_offset of open children
//     
//     if (fnlen != fileref->filepart.Length / sizeof(WCHAR) || RtlCompareMemory(fn, fileref->filepart.Buffer, fileref->filepart.Length) != fileref->filepart.Length) {
//         ExFreePool(fileref->filepart.Buffer);
//         fileref->filepart.Length = fileref->filepart.MaximumLength = (USHORT)(fnlen * sizeof(WCHAR));
//         fileref->filepart.Buffer = ExAllocatePoolWithTag(PagedPool, fileref->filepart.MaximumLength, ALLOC_TAG);
//         
//         if (!fileref->filepart.Buffer) {
//             ERR("out of memory\n");
//             
//             Status = STATUS_INSUFFICIENT_RESOURCES;
//             goto end;
//         }
//         
//         RtlCopyMemory(fileref->filepart.Buffer, fn, fileref->filepart.Length);
//     }
//     
//     if (related && related != (file_ref*)fileref->parent) {
//         file_ref* oldpar = (file_ref*)fileref->parent;
// #ifdef DEBUG_FCB_REFCOUNTS
//         LONG rc;
// #endif
//         
//         RemoveEntryList(&fileref->list_entry);
//       
//         fileref->parent = (struct _file_ref*)related;
// #ifdef DEBUG_FCB_REFCOUNTS
//         rc = InterlockedIncrement(&related->refcount);
//         WARN("fileref %p: refcount now %i (%S)\n", fileref->parent, rc, file_desc_fileref((file_ref*)fileref->parent));
// #else
//         InterlockedIncrement(&related->refcount);
// #endif
//         
//         InsertTailList(&related->children, &fileref->list_entry);
//         
//         free_fileref(oldpar);
//     }
//     
//     ExFreePool(fileref->utf8.Buffer);
//     fileref->utf8 = utf8;
//     utf8.Buffer = NULL;
//     
//     // change fileref->full_filename
//     
//     fileref->full_filename.MaximumLength = fileref->parent->full_filename.Length + fileref->filepart.Length;
//     if (fileref->parent->parent) fileref->full_filename.MaximumLength += sizeof(WCHAR);
//     ExFreePool(fileref->full_filename.Buffer);
//     
//     fileref->full_filename.Buffer = ExAllocatePoolWithTag(PagedPool, fileref->full_filename.MaximumLength, ALLOC_TAG);
//     if (!fileref->full_filename.Buffer) {
//         ERR("out of memory\n");
//         
//         Status = STATUS_INSUFFICIENT_RESOURCES;
//         goto end;
//     }
//     
//     RtlCopyMemory(fileref->full_filename.Buffer, fileref->parent->full_filename.Buffer, fileref->parent->full_filename.Length);
//     fileref->full_filename.Length = fileref->parent->full_filename.Length;
//     
//     if (fileref->parent->parent) {
//         fileref->full_filename.Buffer[fileref->full_filename.Length / sizeof(WCHAR)] = '\\';
//         fileref->full_filename.Length += sizeof(WCHAR);
//     }
//     fileref->name_offset = fileref->full_filename.Length / sizeof(WCHAR);
//     
//     RtlAppendUnicodeStringToString(&fileref->full_filename, &fileref->filepart);
//     
//     if (fileref->debug_desc) {
//         ExFreePool(fileref->debug_desc);
//         fileref->debug_desc = NULL;
//     }
//     
//     send_notification_fileref(fileref, fcb->type == BTRFS_TYPE_DIRECTORY ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
//                               across_directories ? FILE_ACTION_ADDED : FILE_ACTION_RENAMED_NEW_NAME);
//     
//     Status = STATUS_SUCCESS;
//     
// end:
//     if (utf8.Buffer)
//         ExFreePool(utf8.Buffer);
//     
//     if (oldfileref)
//         free_fileref(oldfileref);
//     
//     if (NT_SUCCESS(Status))
//         Status = consider_write(Vcb);
// 
//     if (NT_SUCCESS(Status))
//         clear_rollback(&rollback);
//     else
//         do_rollback(Vcb, &rollback);
// 
//     ExReleaseResourceLite(&Vcb->tree_lock);
//     
//     return Status;
// }

static NTSTATUS STDCALL set_rename_information(device_extension* Vcb, PIRP Irp, PFILE_OBJECT FileObject, PFILE_OBJECT tfo, BOOL ReplaceIfExists) {
    FILE_RENAME_INFORMATION* fri = Irp->AssociatedIrp.SystemBuffer;
    fcb *fcb = FileObject->FsContext;
    ccb* ccb = FileObject->FsContext2;
    file_ref *fileref = ccb ? ccb->fileref : NULL, *oldfileref = NULL, *related = NULL, *fr2 = NULL;
    UINT64 index;
    WCHAR* fn;
    ULONG fnlen, utf8len;
    UNICODE_STRING fnus;
    ANSI_STRING utf8;
    NTSTATUS Status;
    LARGE_INTEGER time;
    BTRFS_TIME now;
    LIST_ENTRY rollback;
    
    InitializeListHead(&rollback);
    
    // FIXME - check fri length
    // FIXME - don't ignore fri->RootDirectory
    
    // FIXME - work with moving subvols
    
    TRACE("tfo = %p\n", tfo);
    TRACE("ReplaceIfExists = %u\n", ReplaceIfExists);
    TRACE("RootDirectory = %p\n", fri->RootDirectory);
    TRACE("FileName = %.*S\n", fri->FileNameLength / sizeof(WCHAR), fri->FileName);
    
    fn = fri->FileName;
    fnlen = fri->FileNameLength / sizeof(WCHAR);
    
    if (!tfo) {
        if (!fileref || !fileref->parent) {
            ERR("no fileref set and no directory given\n");
            return STATUS_INVALID_PARAMETER;
        }
    } else {
        LONG i;
        
        for (i = fnlen - 1; i >= 0; i--) {
            if (fri->FileName[i] == '\\' || fri->FileName[i] == '/') {
                fn = &fri->FileName[i+1];
                fnlen = (fri->FileNameLength / sizeof(WCHAR)) - i - 1;
                break;
            }
        }
    }
    
    ExAcquireResourceSharedLite(&Vcb->tree_lock, TRUE);
    ExAcquireResourceExclusiveLite(fcb->Header.Resource, TRUE);
    
    if (fcb->ads) {
        FIXME("FIXME - renaming streams\n"); // FIXME
        Status = STATUS_NOT_IMPLEMENTED;
        goto end;
    }
    
    fnus.Buffer = fn;
    fnus.Length = fnus.MaximumLength = fnlen * sizeof(WCHAR);
    
    TRACE("fnus = %.*S\n", fnus.Length / sizeof(WCHAR), fnus.Buffer);
    
    Status = RtlUnicodeToUTF8N(NULL, 0, &utf8len, fn, (ULONG)fnlen * sizeof(WCHAR));
    if (!NT_SUCCESS(Status))
        goto end;
    
    utf8.MaximumLength = utf8.Length = utf8len;
    utf8.Buffer = ExAllocatePoolWithTag(PagedPool, utf8.MaximumLength, ALLOC_TAG);
    if (!utf8.Buffer) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    Status = RtlUnicodeToUTF8N(utf8.Buffer, utf8len, &utf8len, fn, (ULONG)fnlen * sizeof(WCHAR));
    if (!NT_SUCCESS(Status))
        goto end;
    
    if (tfo && tfo->FsContext2) {
        struct _ccb* relatedccb = tfo->FsContext2;
        
        related = relatedccb->fileref;
        related->refcount++;
    }

    Status = open_fileref(Vcb, &oldfileref, &fnus, related, FALSE, NULL);

    if (NT_SUCCESS(Status)) {
        WARN("destination file %S already exists\n", file_desc_fileref(oldfileref));
        
        if (fileref != oldfileref && !(oldfileref->fcb->open_count == 0 && oldfileref->deleted)) {
            if (!ReplaceIfExists) {
                Status = STATUS_OBJECT_NAME_COLLISION;
                goto end;
            } else if ((oldfileref->fcb->open_count >= 1 || has_open_children(oldfileref)) && !oldfileref->deleted) {
                WARN("trying to overwrite open file\n");
                Status = STATUS_ACCESS_DENIED;
                goto end;
            }
            
            if (oldfileref->fcb->type == BTRFS_TYPE_DIRECTORY) {
                WARN("trying to overwrite directory\n");
                Status = STATUS_ACCESS_DENIED;
                goto end;
            }
        }
    }
    
    if (!related) {
        Status = open_fileref(Vcb, &related, &fnus, NULL, TRUE, NULL);

        if (!NT_SUCCESS(Status)) {
            ERR("open_fileref returned %08x\n", Status);
            goto end;
        }
    }
    
    if (has_open_children(fileref)) {
        WARN("trying to rename file with open children\n");
        Status = STATUS_ACCESS_DENIED;
        goto end;
    }
    
    if (fileref->parent->fcb->subvol != related->fcb->subvol && fileref->fcb->subvol == fileref->parent->fcb->subvol) {
        FIXME("FIXME - move file across subvols\n"); // FIXME
        Status = STATUS_NOT_IMPLEMENTED;
        goto end;
    }
    
    if (oldfileref) {
        // FIXME - check we have permissions for this
        Status = delete_fileref(oldfileref, NULL, &rollback);
        if (!NT_SUCCESS(Status)) {
            ERR("delete_fileref returned %08x\n", Status);
            goto end;
        }
    }
    
    if (related == fileref->parent) { // keeping file in same directory
        UNICODE_STRING fnus2, ff;
        ULONG oldutf8len;
        
        fnus2.Buffer = ExAllocatePoolWithTag(PagedPool, fnus.Length, ALLOC_TAG);
        if (!fnus2.Buffer) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }
        
        ff.Length = ff.MaximumLength = (fileref->name_offset * sizeof(WCHAR)) + fnus.Length;
        ff.Buffer = ExAllocatePoolWithTag(PagedPool, ff.Length, ALLOC_TAG);
        if (!ff.Buffer) {
            ERR("out of memory\n");
            ExFreePool(fnus2.Buffer);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }
        
        fnus2.Length = fnus2.MaximumLength = fnus.Length;
        RtlCopyMemory(fnus2.Buffer, fnus.Buffer, fnus.Length);
        
        oldutf8len = fileref->utf8.Length;
        
        if (!fileref->created && !fileref->oldutf8.Buffer)
            fileref->oldutf8 = fileref->utf8;
        else
            ExFreePool(fileref->utf8.Buffer);
        
        fileref->utf8 = utf8;
        fileref->filepart = fnus2;
        
        ExFreePool(fileref->full_filename.Buffer);
        
        fileref->full_filename = ff;
        RtlCopyMemory(fileref->full_filename.Buffer, related->full_filename.Buffer, related->full_filename.Length);
        fileref->full_filename.Buffer[related->full_filename.Length / sizeof(WCHAR)] = '\\';
        RtlCopyMemory(&fileref->full_filename.Buffer[fileref->name_offset], fileref->filepart.Buffer, fileref->filepart.Length);
        
        mark_fileref_dirty(fileref);
        
        KeQuerySystemTime(&time);
        win_time_to_unix(time, &now);
        
        fcb->inode_item.transid = Vcb->superblock.generation;
        fcb->inode_item.sequence++;
        fcb->inode_item.st_ctime = now;
        
        mark_fcb_dirty(fcb);
        
        // update parent's INODE_ITEM
        
        related->fcb->inode_item.transid = Vcb->superblock.generation;
        related->fcb->inode_item.st_size = related->fcb->inode_item.st_size + (2 * utf8.Length) - (2* oldutf8len);
        related->fcb->inode_item.sequence++;
        related->fcb->inode_item.st_ctime = now;
        related->fcb->inode_item.st_mtime = now;
        
        mark_fcb_dirty(related->fcb);
        
        Status = STATUS_SUCCESS;
        goto end;
    }
    
    // We move files by moving the existing fileref to the new directory, and
    // replacing it with a dummy fileref with the same original values, but marked as deleted.
    
    fr2 = create_fileref();
    
    fr2->fcb = fileref->fcb;
    fr2->fcb->refcount++;
    
    fr2->filepart = fileref->filepart;
    fr2->utf8 = fileref->utf8;
    fr2->oldutf8 = fileref->oldutf8;
    fr2->full_filename = fileref->full_filename;
    fr2->name_offset = fileref->name_offset;
    fr2->index = fileref->index;
    fr2->delete_on_close = fileref->delete_on_close;
    fr2->deleted = TRUE;
    fr2->created = fileref->created;
    fr2->parent = fileref->parent;

    Status = fcb_get_last_dir_index(related->fcb, &index);
    if (!NT_SUCCESS(Status)) {
        ERR("fcb_get_last_dir_index returned %08x\n", Status);
        goto end;
    }
    
    fileref->filepart.Buffer = ExAllocatePoolWithTag(PagedPool, fnus.Length, ALLOC_TAG);
    if (!fileref->filepart.Buffer) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    fileref->filepart.Length = fileref->filepart.MaximumLength = fnus.Length;
    RtlCopyMemory(fileref->filepart.Buffer, fnus.Buffer, fnus.Length);
    
    fileref->name_offset = related->full_filename.Length / sizeof(WCHAR);

    if (related != Vcb->root_fileref)
        fileref->name_offset++;
    
    fileref->full_filename.Length = fileref->full_filename.MaximumLength = (fileref->name_offset * sizeof(WCHAR)) + fileref->filepart.Length;
    fileref->full_filename.Buffer = ExAllocatePoolWithTag(PagedPool, fileref->full_filename.Length, ALLOC_TAG);
    if (!fileref->full_filename.Buffer) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    RtlCopyMemory(fileref->full_filename.Buffer, related->full_filename.Buffer, related->full_filename.Length);
    
    fileref->full_filename.Buffer[related->full_filename.Length / sizeof(WCHAR)] = '\\';
    
    RtlCopyMemory(&fileref->full_filename.Buffer[fileref->name_offset], fileref->filepart.Buffer, fileref->filepart.Length);

    fileref->utf8 = utf8;
    fileref->oldutf8.Buffer = NULL;
    fileref->index = index;
    fileref->deleted = FALSE;
    fileref->created = TRUE;
    fileref->parent = related;

    ExAcquireResourceExclusiveLite(&fileref->parent->nonpaged->children_lock, TRUE);
    InsertHeadList(&fileref->list_entry, &fr2->list_entry);
    RemoveEntryList(&fileref->list_entry);
    ExReleaseResourceLite(&fileref->parent->nonpaged->children_lock);
    
    insert_fileref_child(related, fileref);
    
    mark_fileref_dirty(fr2);
    mark_fileref_dirty(fileref);

    // update inode's INODE_ITEM
    
    KeQuerySystemTime(&time);
    win_time_to_unix(time, &now);
    
    fcb->inode_item.transid = Vcb->superblock.generation;
    fcb->inode_item.sequence++;
    fcb->inode_item.st_ctime = now;
    
    mark_fcb_dirty(fcb);
    
    // update new parent's INODE_ITEM
    
    related->fcb->inode_item.transid = Vcb->superblock.generation;
    related->fcb->inode_item.st_size += 2 * utf8len;
    related->fcb->inode_item.sequence++;
    related->fcb->inode_item.st_ctime = now;
    related->fcb->inode_item.st_mtime = now;
    
    mark_fcb_dirty(related->fcb);
    
    // update old parent's INODE_ITEM
    
    fr2->parent->fcb->inode_item.transid = Vcb->superblock.generation;
    fr2->parent->fcb->inode_item.st_size -= 2 * fr2->utf8.Length;
    fr2->parent->fcb->inode_item.sequence++;
    fr2->parent->fcb->inode_item.st_ctime = now;
    fr2->parent->fcb->inode_item.st_mtime = now;
    
    free_fileref(fr2);
    
    mark_fcb_dirty(fr2->parent->fcb);
    
    // FIXME - notification

    Status = STATUS_SUCCESS;
    
end:
    if (oldfileref)
        free_fileref(oldfileref);
    
    if (!NT_SUCCESS(Status) && related)
        free_fileref(related);
    
    if (!NT_SUCCESS(Status) && fr2)
        free_fileref(fr2);
    
//     if (NT_SUCCESS(Status))
        clear_rollback(&rollback);
//     else
//         do_rollback(Vcb, &rollback);

    ExReleaseResourceLite(fcb->Header.Resource);
    ExReleaseResourceLite(&Vcb->tree_lock);
    
    return Status;
}

NTSTATUS STDCALL stream_set_end_of_file_information(device_extension* Vcb, UINT64 end, fcb* fcb, file_ref* fileref, PFILE_OBJECT FileObject, BOOL advance_only, LIST_ENTRY* rollback) {
    LARGE_INTEGER time;
    BTRFS_TIME now;
    CC_FILE_SIZES ccfs;
    
    TRACE("setting new end to %llx bytes (currently %x)\n", end, fcb->adsdata.Length);
    
    if (!fileref || !fileref->parent) {
        ERR("no fileref for stream\n");
        return STATUS_INTERNAL_ERROR;
    }
    
    if (end < fcb->adsdata.Length) {
        if (advance_only)
            return STATUS_SUCCESS;
        
        TRACE("truncating stream to %llx bytes\n", end);
        
        fcb->adsdata.Length = end;
    } else if (end > fcb->adsdata.Length) {
//         UINT16 maxlen;
        
        TRACE("extending stream to %llx bytes\n", end);
//         
//         // find maximum length of xattr
//         maxlen = Vcb->superblock.node_size - sizeof(tree_header) - sizeof(leaf_node);
//         
//         searchkey.obj_id = fcb->inode;
//         searchkey.obj_type = TYPE_XATTR_ITEM;
//         searchkey.offset = fcb->adshash;
// 
//         Status = find_item(fcb->Vcb, fcb->subvol, &tp, &searchkey, FALSE);
//         if (!NT_SUCCESS(Status)) {
//             ERR("error - find_item returned %08x\n", Status);
//             return Status;
//         }
//         
//         if (keycmp(&tp.item->key, &searchkey)) {
//             ERR("error - could not find key for xattr\n");
//             return STATUS_INTERNAL_ERROR;
//         }
//         
//         if (!get_xattr(Vcb, fcb->subvol, fcb->inode, fcb->adsxattr.Buffer, fcb->adshash, &data, &datalen)) {
//             ERR("get_xattr failed\n");
//             return STATUS_INTERNAL_ERROR;
//         }
//         
//         maxlen -= tp.item->size - datalen; // subtract XATTR_ITEM overhead
//         
//         if (end > maxlen) {
//             ERR("error - xattr too long (%llu > %u)\n", end, maxlen);
//             return STATUS_DISK_FULL;
//         }

        if (end > fcb->adsdata.MaximumLength) {
            char* data = ExAllocatePoolWithTag(PagedPool, end, ALLOC_TAG);
            if (!data) {
                ERR("out of memory\n");
                ExFreePool(data);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            
            if (fcb->adsdata.Buffer) {
                RtlCopyMemory(data, fcb->adsdata.Buffer, fcb->adsdata.Length);
                ExFreePool(fcb->adsdata.Buffer);
            }
            
            fcb->adsdata.Buffer = data;
            fcb->adsdata.MaximumLength = end;
        }
        
        RtlZeroMemory(&fcb->adsdata.Buffer[fcb->adsdata.Length], end - fcb->adsdata.Length);
        
        fcb->adsdata.Length = end;
    }
    
    mark_fcb_dirty(fcb);

    if (FileObject) {
        ccfs.AllocationSize = fcb->Header.AllocationSize;
        ccfs.FileSize = fcb->Header.FileSize;
        ccfs.ValidDataLength = fcb->Header.ValidDataLength;

        CcSetFileSizes(FileObject, &ccfs);
    }
    
    KeQuerySystemTime(&time);
    win_time_to_unix(time, &now);
    
    fileref->parent->fcb->inode_item.transid = Vcb->superblock.generation;
    fileref->parent->fcb->inode_item.sequence++;
    fileref->parent->fcb->inode_item.st_ctime = now;
    
    mark_fcb_dirty(fileref->parent->fcb);

    fileref->parent->fcb->subvol->root_item.ctransid = Vcb->superblock.generation;
    fileref->parent->fcb->subvol->root_item.ctime = now;

    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL set_end_of_file_information(device_extension* Vcb, PIRP Irp, PFILE_OBJECT FileObject, BOOL advance_only) {
    FILE_END_OF_FILE_INFORMATION* feofi = Irp->AssociatedIrp.SystemBuffer;
    fcb* fcb = FileObject->FsContext;
    ccb* ccb = FileObject->FsContext2;
    file_ref* fileref = ccb ? ccb->fileref : NULL;
    NTSTATUS Status;
    LARGE_INTEGER time;
    CC_FILE_SIZES ccfs;
    LIST_ENTRY rollback;
    
    InitializeListHead(&rollback);
    
    ExAcquireResourceSharedLite(&Vcb->tree_lock, TRUE);
    
    ExAcquireResourceExclusiveLite(fcb->Header.Resource, TRUE);
    
    if (fileref ? fileref->deleted : fcb->deleted) {
        Status = STATUS_FILE_CLOSED;
        goto end;
    }
    
    if (fcb->ads) {
        Status = stream_set_end_of_file_information(Vcb, feofi->EndOfFile.QuadPart, fcb, fileref, FileObject, advance_only, &rollback);
        goto end;
    }
    
    TRACE("file: %S\n", file_desc(FileObject));
    TRACE("paging IO: %s\n", Irp->Flags & IRP_PAGING_IO ? "TRUE" : "FALSE");
    TRACE("FileObject: AllocationSize = %llx, FileSize = %llx, ValidDataLength = %llx\n",
        fcb->Header.AllocationSize.QuadPart, fcb->Header.FileSize.QuadPart, fcb->Header.ValidDataLength.QuadPart);
    
//     int3;
    TRACE("setting new end to %llx bytes (currently %llx)\n", feofi->EndOfFile.QuadPart, fcb->inode_item.st_size);
    
//     if (feofi->EndOfFile.QuadPart==0x36c000)
//         int3;
    
    if (feofi->EndOfFile.QuadPart < fcb->inode_item.st_size) {
        if (advance_only) {
            Status = STATUS_SUCCESS;
            goto end;
        }
        
        TRACE("truncating file to %llx bytes\n", feofi->EndOfFile.QuadPart);
        
        Status = truncate_file(fcb, feofi->EndOfFile.QuadPart, &rollback);
        if (!NT_SUCCESS(Status)) {
            ERR("error - truncate_file failed\n");
            goto end;
        }
    } else if (feofi->EndOfFile.QuadPart > fcb->inode_item.st_size) {
        if (Irp->Flags & IRP_PAGING_IO) {
            TRACE("paging IO tried to extend file size\n");
            Status = STATUS_SUCCESS;
            goto end;
        }
        
        TRACE("extending file to %llx bytes\n", feofi->EndOfFile.QuadPart);
        
        Status = extend_file(fcb, fileref, feofi->EndOfFile.QuadPart, TRUE, &rollback);
        if (!NT_SUCCESS(Status)) {
            ERR("error - extend_file failed\n");
            goto end;
        }
    }
    
    ccfs.AllocationSize = fcb->Header.AllocationSize;
    ccfs.FileSize = fcb->Header.FileSize;
    ccfs.ValidDataLength = fcb->Header.ValidDataLength;

    CcSetFileSizes(FileObject, &ccfs);
    TRACE("setting FileSize for %S to %llx\n", file_desc(FileObject), ccfs.FileSize);
    
    KeQuerySystemTime(&time);
    
    win_time_to_unix(time, &fcb->inode_item.st_mtime);
    
    mark_fcb_dirty(fcb);

    Status = STATUS_SUCCESS;

end:
    if (NT_SUCCESS(Status))
        clear_rollback(&rollback);
    else
        do_rollback(Vcb, &rollback);

    ExReleaseResourceLite(fcb->Header.Resource);
    
    ExReleaseResourceLite(&Vcb->tree_lock);
    
    return Status;
}

// static NTSTATUS STDCALL set_allocation_information(device_extension* Vcb, PIRP Irp, PFILE_OBJECT FileObject) {
//     FILE_ALLOCATION_INFORMATION* fai = (FILE_ALLOCATION_INFORMATION*)Irp->AssociatedIrp.SystemBuffer;
//     fcb* fcb = FileObject->FsContext;
//     
//     FIXME("FIXME\n");
//     ERR("fcb = %p (%.*S)\n", fcb, fcb->full_filename.Length / sizeof(WCHAR), fcb->full_filename.Buffer);
//     ERR("AllocationSize = %llx\n", fai->AllocationSize.QuadPart);
//     
//     return STATUS_NOT_IMPLEMENTED;
// }

static NTSTATUS STDCALL set_position_information(device_extension* Vcb, PIRP Irp, PFILE_OBJECT FileObject) {
    FILE_POSITION_INFORMATION* fpi = (FILE_POSITION_INFORMATION*)Irp->AssociatedIrp.SystemBuffer;
    
    TRACE("setting the position on %S to %llx\n", file_desc(FileObject), fpi->CurrentByteOffset.QuadPart);
    
    // FIXME - make sure aligned for FO_NO_INTERMEDIATE_BUFFERING
    
    FileObject->CurrentByteOffset = fpi->CurrentByteOffset;
    
    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL set_link_information(device_extension* Vcb, PIRP Irp, PFILE_OBJECT FileObject, PFILE_OBJECT tfo) {
    FILE_LINK_INFORMATION* fli = Irp->AssociatedIrp.SystemBuffer;
    fcb *fcb = FileObject->FsContext, *tfofcb, *parfcb;
    ccb* ccb = FileObject->FsContext2;
    file_ref *fileref = ccb ? ccb->fileref : NULL, *oldfileref = NULL, *related = NULL, *fr2 = NULL;
    UINT64 index;
    WCHAR* fn;
    ULONG fnlen, utf8len;
    UNICODE_STRING fnus;
    ANSI_STRING utf8;
    NTSTATUS Status;
    LARGE_INTEGER time;
    BTRFS_TIME now;
    LIST_ENTRY rollback;
    
    InitializeListHead(&rollback);
    
    // FIXME - check fli length
    // FIXME - don't ignore fli->RootDirectory
    
    TRACE("ReplaceIfExists = %x\n", fli->ReplaceIfExists);
    TRACE("RootDirectory = %p\n", fli->RootDirectory);
    TRACE("FileNameLength = %x\n", fli->FileNameLength);
    TRACE("FileName = %.*S\n", fli->FileNameLength / sizeof(WCHAR), fli->FileName);
    
    fn = fli->FileName;
    fnlen = fli->FileNameLength / sizeof(WCHAR);
    
    if (!tfo) {
        if (!fileref || !fileref->parent) {
            ERR("no fileref set and no directory given\n");
            return STATUS_INVALID_PARAMETER;
        }
        
        parfcb = fileref->parent->fcb;
        tfofcb = NULL;
    } else {
        LONG i;
        
        tfofcb = tfo->FsContext;
        parfcb = tfofcb;
        
        for (i = fnlen - 1; i >= 0; i--) {
            if (fli->FileName[i] == '\\' || fli->FileName[i] == '/') {
                fn = &fli->FileName[i+1];
                fnlen = (fli->FileNameLength / sizeof(WCHAR)) - i - 1;
                break;
            }
        }
    }
    
    ExAcquireResourceSharedLite(&Vcb->tree_lock, TRUE);
    ExAcquireResourceExclusiveLite(fcb->Header.Resource, TRUE);
    
    if (fcb->type == BTRFS_TYPE_DIRECTORY) {
        WARN("tried to create hard link on directory\n");
        Status = STATUS_FILE_IS_A_DIRECTORY;
        goto end;
    }
    
    if (fcb->ads) {
        WARN("tried to create hard link on stream\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }
    
    fnus.Buffer = fn;
    fnus.Length = fnus.MaximumLength = fnlen * sizeof(WCHAR);
    
    TRACE("fnus = %.*S\n", fnus.Length / sizeof(WCHAR), fnus.Buffer);
    
    Status = RtlUnicodeToUTF8N(NULL, 0, &utf8len, fn, (ULONG)fnlen * sizeof(WCHAR));
    if (!NT_SUCCESS(Status))
        goto end;
    
    utf8.MaximumLength = utf8.Length = utf8len;
    utf8.Buffer = ExAllocatePoolWithTag(PagedPool, utf8.MaximumLength, ALLOC_TAG);
    if (!utf8.Buffer) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    Status = RtlUnicodeToUTF8N(utf8.Buffer, utf8len, &utf8len, fn, (ULONG)fnlen * sizeof(WCHAR));
    if (!NT_SUCCESS(Status))
        goto end;
    
    if (tfo && tfo->FsContext2) {
        struct _ccb* relatedccb = tfo->FsContext2;
        
        related = relatedccb->fileref;
        related->refcount++;
    }

    Status = open_fileref(Vcb, &oldfileref, &fnus, related, FALSE, NULL);

    if (NT_SUCCESS(Status)) {
        WARN("destination file %S already exists\n", file_desc_fileref(oldfileref));
        
        if (fileref != oldfileref && !(oldfileref->fcb->open_count == 0 && oldfileref->deleted)) {
            if (!fli->ReplaceIfExists) {
                Status = STATUS_OBJECT_NAME_COLLISION;
                goto end;
            } else if (oldfileref->fcb->open_count >= 1 && !oldfileref->deleted) {
                WARN("trying to overwrite open file\n");
                Status = STATUS_ACCESS_DENIED;
                goto end;
            }
            
            if (oldfileref->fcb->type == BTRFS_TYPE_DIRECTORY) {
                WARN("trying to overwrite directory\n");
                Status = STATUS_ACCESS_DENIED;
                goto end;
            }
        }
    }
    
    if (!related) {
        Status = open_fileref(Vcb, &related, &fnus, NULL, TRUE, NULL);

        if (!NT_SUCCESS(Status)) {
            ERR("open_fileref returned %08x\n", Status);
            goto end;
        }
    }
    
    if (fcb->subvol != parfcb->subvol) {
        WARN("can't create hard link over subvolume boundary\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }
    
    if (oldfileref) {
        // FIXME - check we have permissions for this
        Status = delete_fileref(oldfileref, NULL, &rollback);
        if (!NT_SUCCESS(Status)) {
            ERR("delete_fileref returned %08x\n", Status);
            goto end;
        }
    }
    
    Status = fcb_get_last_dir_index(related->fcb, &index);
    if (!NT_SUCCESS(Status)) {
        ERR("fcb_get_last_dir_index returned %08x\n", Status);
        goto end;
    }
    
    fr2 = create_fileref();
    
    fr2->fcb = fcb;
    fcb->refcount++;
    
    fr2->utf8 = utf8;
    fr2->index = index;
    fr2->created = TRUE;
    fr2->parent = related;
    
    fr2->filepart.Buffer = ExAllocatePoolWithTag(PagedPool, fnus.Length, ALLOC_TAG);
    if (!fr2->filepart.Buffer) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    fr2->filepart.Length = fr2->filepart.MaximumLength = fnus.Length;
    RtlCopyMemory(fr2->filepart.Buffer, fnus.Buffer, fnus.Length);
    
    fr2->name_offset = related->full_filename.Length / sizeof(WCHAR);

    if (related != Vcb->root_fileref)
        fr2->name_offset++;
    
    fr2->full_filename.Length = fr2->full_filename.MaximumLength = (fr2->name_offset * sizeof(WCHAR)) + fr2->filepart.Length;
    fr2->full_filename.Buffer = ExAllocatePoolWithTag(PagedPool, fr2->full_filename.Length, ALLOC_TAG);
    if (!fr2->full_filename.Buffer) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    RtlCopyMemory(fr2->full_filename.Buffer, related->full_filename.Buffer, related->full_filename.Length);
    
    fr2->full_filename.Buffer[related->full_filename.Length / sizeof(WCHAR)] = '\\';
    
    RtlCopyMemory(&fr2->full_filename.Buffer[fr2->name_offset], fr2->filepart.Buffer, fr2->filepart.Length);
    
    insert_fileref_child(related, fr2);
    
    mark_fileref_dirty(fr2);
    free_fileref(fr2);
    
    // update inode's INODE_ITEM
    
    KeQuerySystemTime(&time);
    win_time_to_unix(time, &now);
    
    fcb->inode_item.transid = Vcb->superblock.generation;
    fcb->inode_item.sequence++;
    fcb->inode_item.st_nlink++;
    fcb->inode_item.st_ctime = now;
    
    mark_fcb_dirty(fcb);
    
    // update parent's INODE_ITEM
    
    parfcb->inode_item.transid = Vcb->superblock.generation;
    parfcb->inode_item.st_size += 2 * utf8len;
    parfcb->inode_item.sequence++;
    parfcb->inode_item.st_ctime = now;
    
    mark_fcb_dirty(parfcb);
    
    // FIXME - notification

    Status = STATUS_SUCCESS;
    
end:
    if (oldfileref)
        free_fileref(oldfileref);
    
    if (!NT_SUCCESS(Status) && related)
        free_fileref(related);
    
    if (!NT_SUCCESS(Status) && fr2)
        free_fileref(fr2);
    
//     if (NT_SUCCESS(Status))
        clear_rollback(&rollback);
//     else
//         do_rollback(Vcb, &rollback);

    ExReleaseResourceLite(fcb->Header.Resource);
    ExReleaseResourceLite(&Vcb->tree_lock);
    
    return Status;
}

NTSTATUS STDCALL drv_set_information(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp) {
    NTSTATUS Status;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    device_extension* Vcb = DeviceObject->DeviceExtension;
    fcb* fcb = IrpSp->FileObject->FsContext;
    BOOL top_level;

    FsRtlEnterFileSystem();
    
    top_level = is_top_level(Irp);
    
    if (Vcb && Vcb->type == VCB_TYPE_PARTITION0) {
        Status = part0_passthrough(DeviceObject, Irp);
        goto exit;
    }
    
    if (Vcb->readonly) {
        Status = STATUS_MEDIA_WRITE_PROTECTED;
        goto end;
    }
    
    if (fcb->subvol->root_item.flags & BTRFS_SUBVOL_READONLY) {
        Status = STATUS_ACCESS_DENIED;
        goto end;
    }

    Irp->IoStatus.Information = 0;
    
    Status = STATUS_NOT_IMPLEMENTED;

    TRACE("set information\n");

    switch (IrpSp->Parameters.SetFile.FileInformationClass) {
        case FileAllocationInformation:
            TRACE("FileAllocationInformation\n");
            Status = set_end_of_file_information(Vcb, Irp, IrpSp->FileObject, FALSE);
            break;

        case FileBasicInformation:
            TRACE("FileBasicInformation\n");
            Status = set_basic_information(Vcb, Irp, IrpSp->FileObject);
            break;

        case FileDispositionInformation:
            TRACE("FileDispositionInformation\n");
            Status = set_disposition_information(Vcb, Irp, IrpSp->FileObject);
            break;

        case FileEndOfFileInformation:
            TRACE("FileEndOfFileInformation\n");
            Status = set_end_of_file_information(Vcb, Irp, IrpSp->FileObject, IrpSp->Parameters.SetFile.AdvanceOnly);
            break;

        case FileLinkInformation:
            TRACE("FileLinkInformation\n");
            Status = set_link_information(Vcb, Irp, IrpSp->FileObject, IrpSp->Parameters.SetFile.FileObject);
            break;

        case FilePositionInformation:
            TRACE("FilePositionInformation\n");
            Status = set_position_information(Vcb, Irp, IrpSp->FileObject);
            break;

        case FileRenameInformation:
            TRACE("FileRenameInformation\n");
            // FIXME - make this work with streams
            Status = set_rename_information(Vcb, Irp, IrpSp->FileObject, IrpSp->Parameters.SetFile.FileObject, IrpSp->Parameters.SetFile.ReplaceIfExists);
            break;

        case FileValidDataLengthInformation:
            FIXME("STUB: FileValidDataLengthInformation\n");
            break;
            
        case FileNormalizedNameInformation:
            FIXME("STUB: FileNormalizedNameInformation\n");
            break;
            
        case FileStandardLinkInformation:
            FIXME("STUB: FileStandardLinkInformation\n");
            break;
            
        case FileRemoteProtocolInformation:
            TRACE("FileRemoteProtocolInformation\n");
            break;
            
        default:
            WARN("unknown FileInformationClass %u\n", IrpSp->Parameters.SetFile.FileInformationClass);
    }
    
end:
    Irp->IoStatus.Status = Status;

    IoCompleteRequest( Irp, IO_NO_INCREMENT );
    
exit:
    if (top_level) 
        IoSetTopLevelIrp(NULL);
    
    FsRtlExitFileSystem();

    return Status;
}

static NTSTATUS STDCALL fill_in_file_basic_information(FILE_BASIC_INFORMATION* fbi, INODE_ITEM* ii, LONG* length, fcb* fcb, file_ref* fileref) {
    RtlZeroMemory(fbi, sizeof(FILE_BASIC_INFORMATION));
    
    *length -= sizeof(FILE_BASIC_INFORMATION);
    
    fbi->CreationTime.QuadPart = unix_time_to_win(&ii->otime);
    fbi->LastAccessTime.QuadPart = unix_time_to_win(&ii->st_atime);
    fbi->LastWriteTime.QuadPart = unix_time_to_win(&ii->st_mtime);
    fbi->ChangeTime.QuadPart = 0;
    
    if (fcb->ads) {
        if (!fileref || !fileref->parent) {
            ERR("no fileref for stream\n");
            return STATUS_INTERNAL_ERROR;
        } else
            fbi->FileAttributes = fileref->parent->fcb->atts;
    } else
        fbi->FileAttributes = fcb->atts;
    
    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL fill_in_file_network_open_information(FILE_NETWORK_OPEN_INFORMATION* fnoi, fcb* fcb, file_ref* fileref, LONG* length) {
    INODE_ITEM* ii;
    
    if (*length < sizeof(FILE_NETWORK_OPEN_INFORMATION)) {
        WARN("overflow\n");
        return STATUS_BUFFER_OVERFLOW;
    }
    
    RtlZeroMemory(fnoi, sizeof(FILE_NETWORK_OPEN_INFORMATION));
    
    *length -= sizeof(FILE_NETWORK_OPEN_INFORMATION);
    
    if (fcb->ads) {
        if (!fileref || !fileref->parent) {
            ERR("no fileref for stream\n");
            return STATUS_INTERNAL_ERROR;
        }
        
        ii = &fileref->parent->fcb->inode_item;
    } else
        ii = &fcb->inode_item;
    
    fnoi->CreationTime.QuadPart = unix_time_to_win(&ii->otime);
    fnoi->LastAccessTime.QuadPart = unix_time_to_win(&ii->st_atime);
    fnoi->LastWriteTime.QuadPart = unix_time_to_win(&ii->st_mtime);
    fnoi->ChangeTime.QuadPart = 0;
    
    if (fcb->ads) {
        fnoi->AllocationSize.QuadPart = fnoi->EndOfFile.QuadPart = fcb->adsdata.Length;
        fnoi->FileAttributes = fileref->parent->fcb->atts;
    } else {
        fnoi->AllocationSize.QuadPart = S_ISDIR(fcb->inode_item.st_mode) ? 0 : sector_align(fcb->inode_item.st_size, fcb->Vcb->superblock.sector_size);
        fnoi->EndOfFile.QuadPart = S_ISDIR(fcb->inode_item.st_mode) ? 0 : fcb->inode_item.st_size;
        fnoi->FileAttributes = fcb->atts;
    }
    
    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL fill_in_file_standard_information(FILE_STANDARD_INFORMATION* fsi, fcb* fcb, file_ref* fileref, LONG* length) {
    RtlZeroMemory(fsi, sizeof(FILE_STANDARD_INFORMATION));
    
    *length -= sizeof(FILE_STANDARD_INFORMATION);
    
    if (fcb->ads) {
        if (!fileref || !fileref->parent) {
            ERR("no fileref for stream\n");
            return STATUS_INTERNAL_ERROR;
        }
        
        fsi->AllocationSize.QuadPart = fsi->EndOfFile.QuadPart = fcb->adsdata.Length;
        fsi->NumberOfLinks = fileref->parent->fcb->inode_item.st_nlink;
        fsi->Directory = S_ISDIR(fileref->parent->fcb->inode_item.st_mode);
    } else {
        fsi->AllocationSize.QuadPart = S_ISDIR(fcb->inode_item.st_mode) ? 0 : sector_align(fcb->inode_item.st_size, fcb->Vcb->superblock.sector_size);
        fsi->EndOfFile.QuadPart = S_ISDIR(fcb->inode_item.st_mode) ? 0 : fcb->inode_item.st_size;
        fsi->NumberOfLinks = fcb->inode_item.st_nlink;
        fsi->Directory = S_ISDIR(fcb->inode_item.st_mode);
    }
    
    TRACE("length = %llu\n", fsi->EndOfFile.QuadPart);
    
    fsi->DeletePending = fileref ? fileref->delete_on_close : FALSE;
    
    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL fill_in_file_internal_information(FILE_INTERNAL_INFORMATION* fii, UINT64 inode, LONG* length) {
    *length -= sizeof(FILE_INTERNAL_INFORMATION);
    
    fii->IndexNumber.QuadPart = inode;
    
    return STATUS_SUCCESS;
}  
    
static NTSTATUS STDCALL fill_in_file_ea_information(FILE_EA_INFORMATION* eai, LONG* length) {
    *length -= sizeof(FILE_EA_INFORMATION);
    
    // FIXME - should this be the reparse tag for symlinks?
    eai->EaSize = 0;
    
    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL fill_in_file_access_information(FILE_ACCESS_INFORMATION* fai, LONG* length) {
    *length -= sizeof(FILE_ACCESS_INFORMATION);
    
    fai->AccessFlags = GENERIC_READ;
    
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS STDCALL fill_in_file_position_information(FILE_POSITION_INFORMATION* fpi, PFILE_OBJECT FileObject, LONG* length) {
    RtlZeroMemory(fpi, sizeof(FILE_POSITION_INFORMATION));
    
    *length -= sizeof(FILE_POSITION_INFORMATION);
    
    fpi->CurrentByteOffset = FileObject->CurrentByteOffset;
    
    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL fill_in_file_mode_information(FILE_MODE_INFORMATION* fmi, ccb* ccb, LONG* length) {
    RtlZeroMemory(fmi, sizeof(FILE_MODE_INFORMATION));
    
    *length -= sizeof(FILE_MODE_INFORMATION);
    
    if (ccb->options & FILE_WRITE_THROUGH)
        fmi->Mode |= FILE_WRITE_THROUGH;
    
    if (ccb->options & FILE_SEQUENTIAL_ONLY)
        fmi->Mode |= FILE_SEQUENTIAL_ONLY;
    
    if (ccb->options & FILE_NO_INTERMEDIATE_BUFFERING)
        fmi->Mode |= FILE_NO_INTERMEDIATE_BUFFERING;
    
    if (ccb->options & FILE_SYNCHRONOUS_IO_ALERT)
        fmi->Mode |= FILE_SYNCHRONOUS_IO_ALERT;
    
    if (ccb->options & FILE_SYNCHRONOUS_IO_NONALERT)
        fmi->Mode |= FILE_SYNCHRONOUS_IO_NONALERT;
    
    if (ccb->options & FILE_DELETE_ON_CLOSE)
        fmi->Mode |= FILE_DELETE_ON_CLOSE;
        
    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL fill_in_file_alignment_information(FILE_ALIGNMENT_INFORMATION* fai, device_extension* Vcb, LONG* length) {
    RtlZeroMemory(fai, sizeof(FILE_ALIGNMENT_INFORMATION));
    
    *length -= sizeof(FILE_ALIGNMENT_INFORMATION);
    
    fai->AlignmentRequirement = Vcb->devices[0].devobj->AlignmentRequirement;
    
    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL fill_in_file_name_information(FILE_NAME_INFORMATION* fni, fcb* fcb, file_ref* fileref, LONG* length) {
#ifdef _DEBUG
    ULONG retlen = 0;
#endif
    static WCHAR datasuf[] = {':','$','D','A','T','A',0};
    ULONG datasuflen = wcslen(datasuf) * sizeof(WCHAR);
    
    if (!fileref) {
        ERR("called without fileref\n");
        return STATUS_INVALID_PARAMETER;
    }
    
    RtlZeroMemory(fni, sizeof(FILE_NAME_INFORMATION));
    
    *length -= (LONG)offsetof(FILE_NAME_INFORMATION, FileName[0]);
    
    TRACE("maximum length is %u\n", *length);
    fni->FileNameLength = 0;
    
    fni->FileName[0] = 0;
    
    if (*length >= (LONG)fileref->full_filename.Length) {
        RtlCopyMemory(fni->FileName, fileref->full_filename.Buffer, fileref->full_filename.Length);
#ifdef _DEBUG
        retlen = fileref->full_filename.Length;
#endif
        *length -= fileref->full_filename.Length;
    } else {
        if (*length > 0) {
            RtlCopyMemory(fni->FileName, fileref->full_filename.Buffer, *length);
#ifdef _DEBUG
            retlen = *length;
#endif
        }
        *length = -1;
    }
    
    fni->FileNameLength = fileref->full_filename.Length;
    
    if (fcb->ads) {
        if (*length >= (LONG)datasuflen) {
            RtlCopyMemory(&fni->FileName[fileref->full_filename.Length / sizeof(WCHAR)], datasuf, datasuflen);
#ifdef _DEBUG
            retlen += datasuflen;
#endif
            *length -= datasuflen;
        } else {
            if (*length > 0) {
                RtlCopyMemory(&fni->FileName[fileref->full_filename.Length / sizeof(WCHAR)], datasuf, *length);
#ifdef _DEBUG
                retlen += *length;
#endif
            }
            *length = -1;
        }
    }
    
    TRACE("%.*S\n", retlen / sizeof(WCHAR), fni->FileName);

    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL fill_in_file_attribute_information(FILE_ATTRIBUTE_TAG_INFORMATION* ati, fcb* fcb, file_ref* fileref, LONG* length) {
    *length -= sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
    
    if (fcb->ads) {
        if (!fileref || !fileref->parent) {
            ERR("no fileref for stream\n");
            return STATUS_INTERNAL_ERROR;
        }
        
        ati->FileAttributes = fileref->parent->fcb->atts;
    } else
        ati->FileAttributes = fcb->atts;
    
    ati->ReparseTag = 0; // FIXME
    
    return STATUS_SUCCESS;
}

typedef struct {
    UNICODE_STRING name;
    UINT64 size;
    BOOL ignore;
    LIST_ENTRY list_entry;
} stream_info;

static NTSTATUS STDCALL fill_in_file_stream_information(FILE_STREAM_INFORMATION* fsi, fcb* fcb, LONG* length) {
    ULONG reqsize;
    LIST_ENTRY streamlist, *le;
    FILE_STREAM_INFORMATION *entry, *lastentry;
    NTSTATUS Status;
    KEY searchkey;
    traverse_ptr tp, next_tp;
    BOOL b;
    stream_info* si;
    
    static WCHAR datasuf[] = {':','$','D','A','T','A',0};
    static char xapref[] = "user.";
    UNICODE_STRING suf;
    
    InitializeListHead(&streamlist);
    
    ExAcquireResourceSharedLite(&fcb->Vcb->tree_lock, TRUE);
    ExAcquireResourceSharedLite(fcb->Header.Resource, TRUE);
    
    suf.Buffer = datasuf;
    suf.Length = suf.MaximumLength = wcslen(datasuf) * sizeof(WCHAR);
    
    searchkey.obj_id = fcb->inode;
    searchkey.obj_type = TYPE_XATTR_ITEM;
    searchkey.offset = 0;

    Status = find_item(fcb->Vcb, fcb->subvol, &tp, &searchkey, FALSE);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08x\n", Status);
        goto end;
    }
    
    reqsize = 0;
    
    si = ExAllocatePoolWithTag(PagedPool, sizeof(stream_info), ALLOC_TAG);
    if (!si) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    si->name.Length = si->name.MaximumLength = 0;
    si->name.Buffer = NULL;
    si->size = fcb->inode_item.st_size;
    si->ignore = FALSE;
    reqsize += sizeof(FILE_STREAM_INFORMATION) - sizeof(WCHAR) + suf.Length + sizeof(WCHAR) + si->name.Length;
    
    InsertTailList(&streamlist, &si->list_entry);
    
    do {
        if (tp.item->key.obj_id == fcb->inode && tp.item->key.obj_type == TYPE_XATTR_ITEM) {
            if (tp.item->size < sizeof(DIR_ITEM)) {
                ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(DIR_ITEM));
            } else {
                ULONG len = tp.item->size;
                DIR_ITEM* xa = (DIR_ITEM*)tp.item->data;
                ULONG stringlen;
                
                do {
                    if (len < sizeof(DIR_ITEM) || len < sizeof(DIR_ITEM) - 1 + xa->m + xa->n) {
                        ERR("(%llx,%x,%llx) was truncated\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset);
                        break;
                    }
                    
                    if (xa->n > strlen(xapref) && RtlCompareMemory(xa->name, xapref, strlen(xapref)) == strlen(xapref) &&
                        (tp.item->key.offset != EA_DOSATTRIB_HASH || xa->n != strlen(EA_DOSATTRIB) || RtlCompareMemory(xa->name, EA_DOSATTRIB, xa->n) != xa->n)) {
                        Status = RtlUTF8ToUnicodeN(NULL, 0, &stringlen, &xa->name[strlen(xapref)], xa->n - strlen(xapref));
                        if (!NT_SUCCESS(Status)) {
                            ERR("RtlUTF8ToUnicodeN 1 returned %08x\n", Status);
                            goto end;
                        }
                        
                        si = ExAllocatePoolWithTag(PagedPool, sizeof(stream_info), ALLOC_TAG);
                        if (!si) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            goto end;
                        }
                        
                        si->name.Buffer = ExAllocatePoolWithTag(PagedPool, stringlen, ALLOC_TAG);
                        if (!si->name.Buffer) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(si);
                            goto end;
                        }
                            
                        Status = RtlUTF8ToUnicodeN(si->name.Buffer, stringlen, &stringlen, &xa->name[strlen(xapref)], xa->n - strlen(xapref));
                        
                        if (!NT_SUCCESS(Status)) {
                            ERR("RtlUTF8ToUnicodeN 2 returned %08x\n", Status);
                            ExFreePool(si->name.Buffer);
                            ExFreePool(si);
                            goto end;
                        }
                        
                        si->name.Length = si->name.MaximumLength = stringlen;
                        
                        si->size = xa->m;
                        reqsize = sector_align(reqsize, sizeof(LONGLONG));
                        reqsize += sizeof(FILE_STREAM_INFORMATION) - sizeof(WCHAR) + suf.Length + sizeof(WCHAR) + si->name.Length;
                        
                        si->ignore = FALSE;
                        
                        TRACE("stream name = %.*S (length = %u)\n", si->name.Length / sizeof(WCHAR), si->name.Buffer, si->name.Length / sizeof(WCHAR));
                        
                        InsertTailList(&streamlist, &si->list_entry);
                    }
                    
                    len -= sizeof(DIR_ITEM) - sizeof(char) + xa->n + xa->m;
                    xa = (DIR_ITEM*)&xa->name[xa->n + xa->m]; // FIXME - test xattr hash collisions work
                } while (len > 0);
            }
        }
        
        b = find_next_item(fcb->Vcb, &tp, &next_tp, FALSE);
        if (b) {
            tp = next_tp;
            
            if (next_tp.item->key.obj_id > fcb->inode || next_tp.item->key.obj_type > TYPE_XATTR_ITEM)
                break;
        }
    } while (b);
    
    // FIXME - loop through open filerefs
    
    TRACE("length = %i, reqsize = %u\n", *length, reqsize);
    
    if (reqsize > *length) {
        Status = STATUS_BUFFER_OVERFLOW;
        goto end;
    }
    
    entry = fsi;
    lastentry = NULL;
    
    le = streamlist.Flink;
    while (le != &streamlist) {
        si = CONTAINING_RECORD(le, stream_info, list_entry);
        
        if (!si->ignore) {
            ULONG off;
            
            entry->NextEntryOffset = 0;
            entry->StreamNameLength = si->name.Length + suf.Length + sizeof(WCHAR);
            entry->StreamSize.QuadPart = si->size;
            
            if (le == streamlist.Flink)
                entry->StreamAllocationSize.QuadPart = sector_align(fcb->inode_item.st_size, fcb->Vcb->superblock.sector_size);
            else
                entry->StreamAllocationSize.QuadPart = si->size;
            
            entry->StreamName[0] = ':';
            
            if (si->name.Length > 0)
                RtlCopyMemory(&entry->StreamName[1], si->name.Buffer, si->name.Length);
            
            RtlCopyMemory(&entry->StreamName[1 + (si->name.Length / sizeof(WCHAR))], suf.Buffer, suf.Length);
            
            if (lastentry)
                lastentry->NextEntryOffset = (UINT8*)entry - (UINT8*)lastentry;
            
            off = sector_align(sizeof(FILE_STREAM_INFORMATION) - sizeof(WCHAR) + suf.Length + sizeof(WCHAR) + si->name.Length, sizeof(LONGLONG));

            lastentry = entry;
            entry = (FILE_STREAM_INFORMATION*)((UINT8*)entry + off);
        }
        
        le = le->Flink;
    }
    
    *length -= reqsize;
    
    Status = STATUS_SUCCESS;
    
end:
    while (!IsListEmpty(&streamlist)) {
        le = RemoveHeadList(&streamlist);
        si = CONTAINING_RECORD(le, stream_info, list_entry);
        
        if (si->name.Buffer)
            ExFreePool(si->name.Buffer);
        
        ExFreePool(si);
    }
    
    ExReleaseResourceLite(fcb->Header.Resource);
    ExReleaseResourceLite(&fcb->Vcb->tree_lock);
    
    return Status;
}

static NTSTATUS STDCALL fill_in_file_standard_link_information(FILE_STANDARD_LINK_INFORMATION* fsli, fcb* fcb, file_ref* fileref, LONG* length) {
    TRACE("FileStandardLinkInformation\n");
    
    // FIXME - NumberOfAccessibleLinks should subtract open links which have been marked as delete_on_close
    
    fsli->NumberOfAccessibleLinks = fcb->inode_item.st_nlink;
    fsli->TotalNumberOfLinks = fcb->inode_item.st_nlink;
    fsli->DeletePending = fileref ? fileref->delete_on_close : FALSE;
    fsli->Directory = fcb->type == BTRFS_TYPE_DIRECTORY ? TRUE : FALSE;
    
    *length -= sizeof(FILE_STANDARD_LINK_INFORMATION);
    
    return STATUS_SUCCESS;
}

typedef struct {
    UNICODE_STRING name;
    UINT64 inode;
    LIST_ENTRY list_entry;
} name_bit;

static NTSTATUS get_inode_dir_path(device_extension* Vcb, root* subvol, UINT64 inode, PUNICODE_STRING us);

static NTSTATUS get_subvol_path(device_extension* Vcb, root* subvol) {
    KEY searchkey;
    traverse_ptr tp;
    NTSTATUS Status;
    LIST_ENTRY* le;
    root* parsubvol;
    UNICODE_STRING dirpath;
    ROOT_REF* rr;
    ULONG namelen;
    
    // FIXME - add subvol->parent field
    
    if (subvol == Vcb->root_fileref->fcb->subvol) {
        subvol->path.Length = subvol->path.MaximumLength = sizeof(WCHAR);
        subvol->path.Buffer = ExAllocatePoolWithTag(PagedPool, subvol->path.Length, ALLOC_TAG);
        subvol->path.Buffer[0] = '\\';
        return STATUS_SUCCESS;
    }
    
    searchkey.obj_id = subvol->id;
    searchkey.obj_type = TYPE_ROOT_BACKREF;
    searchkey.offset = 0xffffffffffffffff;
    
    Status = find_item(Vcb, Vcb->root_root, &tp, &searchkey, FALSE);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08x\n", Status);
        return Status;
    }
    
    if (tp.item->key.obj_id != searchkey.obj_id || tp.item->key.obj_type != searchkey.obj_type) { // top subvol
        subvol->path.Length = subvol->path.MaximumLength = sizeof(WCHAR);
        subvol->path.Buffer = ExAllocatePoolWithTag(PagedPool, subvol->path.Length, ALLOC_TAG);
        subvol->path.Buffer[0] = '\\';
        return STATUS_SUCCESS;
    }
    
    if (tp.item->size < sizeof(ROOT_REF)) {
        ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(ROOT_REF));
        return STATUS_INTERNAL_ERROR;
    }
    
    rr = (ROOT_REF*)tp.item->data;
    
    if (tp.item->size < sizeof(ROOT_REF) - 1 + rr->n) {
        ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(ROOT_REF) - 1 + rr->n);
        return STATUS_INTERNAL_ERROR;
    }
    
    le = Vcb->roots.Flink;

    parsubvol = NULL;

    while (le != &Vcb->roots) {
        root* r2 = CONTAINING_RECORD(le, root, list_entry);
        
        if (r2->id == tp.item->key.offset) {
            parsubvol = r2;
            break;
        }
        
        le = le->Flink;
    }
    
    if (!parsubvol) {
        ERR("unable to find subvol %llx\n", tp.item->key.offset);
        return STATUS_INTERNAL_ERROR;
    }
    
    // FIXME - recursion

    Status = get_inode_dir_path(Vcb, parsubvol, rr->dir, &dirpath);
    if (!NT_SUCCESS(Status)) {
        ERR("get_inode_dir_path returned %08x\n", Status);
        return Status;
    }
    
    Status = RtlUTF8ToUnicodeN(NULL, 0, &namelen, rr->name, rr->n);
    if (!NT_SUCCESS(Status)) {
        ERR("RtlUTF8ToUnicodeN returned %08x\n", Status);
        goto end;
    }
    
    if (namelen == 0) {
        ERR("length was 0\n");
        Status = STATUS_INTERNAL_ERROR;
        goto end;
    }
    
    subvol->path.Length = subvol->path.MaximumLength = dirpath.Length + namelen;
    subvol->path.Buffer = ExAllocatePoolWithTag(PagedPool, subvol->path.Length, ALLOC_TAG);
    
    if (!subvol->path.Buffer) {  
        ERR("out of memory\n");
        
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    RtlCopyMemory(subvol->path.Buffer, dirpath.Buffer, dirpath.Length);
        
    Status = RtlUTF8ToUnicodeN(&subvol->path.Buffer[dirpath.Length / sizeof(WCHAR)], namelen, &namelen, rr->name, rr->n);
    if (!NT_SUCCESS(Status)) {
        ERR("RtlUTF8ToUnicodeN returned %08x\n", Status);
        goto end;
    }
    
    Status = STATUS_SUCCESS;
    
end:
    if (dirpath.Buffer)
        ExFreePool(dirpath.Buffer);
    
    if (!NT_SUCCESS(Status) && subvol->path.Buffer) {
        ExFreePool(subvol->path.Buffer);
        subvol->path.Buffer = NULL;
    }
    
    return Status;
}

static NTSTATUS get_inode_dir_path(device_extension* Vcb, root* subvol, UINT64 inode, PUNICODE_STRING us) {
    KEY searchkey;
    NTSTATUS Status;
    UINT64 in;
    traverse_ptr tp;
    LIST_ENTRY name_trail, *le;
    UINT16 levels = 0;
    UINT32 namelen = 0;
    WCHAR* usbuf;
    
    InitializeListHead(&name_trail);
    
    in = inode;
    
    // FIXME - start with subvol prefix
    if (!subvol->path.Buffer) {
        Status = get_subvol_path(Vcb, subvol);
        if (!NT_SUCCESS(Status)) {
            ERR("get_subvol_path returned %08x\n", Status);
            return Status;
        }
    }
    
    while (in != subvol->root_item.objid) {
        searchkey.obj_id = in;
        searchkey.obj_type = TYPE_INODE_EXTREF;
        searchkey.offset = 0xffffffffffffffff;
        
        Status = find_item(Vcb, subvol, &tp, &searchkey, FALSE);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08x\n", Status);
            goto end;
        }
        
        if (tp.item->key.obj_id != searchkey.obj_id) {
            ERR("could not find INODE_REF for inode %llx in subvol %llx\n", searchkey.obj_id, subvol->id);
            Status = STATUS_INTERNAL_ERROR;
            goto end;
        }
        
        if (tp.item->key.obj_type == TYPE_INODE_REF) {
            INODE_REF* ir = (INODE_REF*)tp.item->data;
            name_bit* nb;
            ULONG len;
            
            if (tp.item->size < sizeof(INODE_REF)) {
                ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(INODE_REF));
                Status = STATUS_INTERNAL_ERROR;
                goto end;
            }
            
            if (tp.item->size < sizeof(INODE_REF) - 1 + ir->n) {
                ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(INODE_REF) - 1 + ir->n);
                Status = STATUS_INTERNAL_ERROR;
                goto end;
            }
            
            nb = ExAllocatePoolWithTag(PagedPool, sizeof(name_bit), ALLOC_TAG);
            if (!nb) {
                ERR("out of memory\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }
            
            nb->name.Buffer = NULL;
            
            InsertTailList(&name_trail, &nb->list_entry);
            levels++;
            
            Status = RtlUTF8ToUnicodeN(NULL, 0, &len, ir->name, ir->n);
            if (!NT_SUCCESS(Status)) {
                ERR("RtlUTF8ToUnicodeN returned %08x\n", Status);
                goto end;
            }
            
            if (len == 0) {
                ERR("length was 0\n");
                Status = STATUS_INTERNAL_ERROR;
                goto end;
            }
            
            nb->name.Length = nb->name.MaximumLength = len;
            
            nb->name.Buffer = ExAllocatePoolWithTag(PagedPool, nb->name.Length, ALLOC_TAG);
            if (!nb->name.Buffer) {  
                ERR("out of memory\n");
                
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }
                
            Status = RtlUTF8ToUnicodeN(nb->name.Buffer, len, &len, ir->name, ir->n);
            if (!NT_SUCCESS(Status)) {
                ERR("RtlUTF8ToUnicodeN returned %08x\n", Status);
                goto end;
            }
            
            in = tp.item->key.offset;
            namelen += nb->name.Length;
            
//         } else if (tp.item->key.obj_type == TYPE_INODE_EXTREF) {
//             // FIXME
        } else {
            ERR("could not find INODE_REF for inode %llx in subvol %llx\n", searchkey.obj_id, subvol->id);
            Status = STATUS_INTERNAL_ERROR;
            goto end;
        }
    }
    
    namelen += (levels + 1) * sizeof(WCHAR);
    
    us->Length = us->MaximumLength = namelen;
    us->Buffer = ExAllocatePoolWithTag(PagedPool, us->Length, ALLOC_TAG);
    
    if (!us->Buffer) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    us->Buffer[0] = '\\';
    usbuf = &us->Buffer[1];
    
    le = name_trail.Blink;
    while (le != &name_trail) {
        name_bit* nb = CONTAINING_RECORD(le, name_bit, list_entry);
        
        RtlCopyMemory(usbuf, nb->name.Buffer, nb->name.Length);
        usbuf += nb->name.Length / sizeof(WCHAR);
        
        usbuf[0] = '\\';
        usbuf++;
        
        le = le->Blink;
    }
    
    Status = STATUS_SUCCESS;
    
end:
    while (!IsListEmpty(&name_trail)) {
        name_bit* nb = CONTAINING_RECORD(name_trail.Flink, name_bit, list_entry);
        
        if (nb->name.Buffer)
            ExFreePool(nb->name.Buffer);
        
        RemoveEntryList(&nb->list_entry);
        
        ExFreePool(nb);
    }

    return Status;
}

static NTSTATUS STDCALL fill_in_hard_link_information(FILE_LINKS_INFORMATION* fli, fcb* fcb, LONG* length) {
    KEY searchkey;
    traverse_ptr tp, next_tp;
    NTSTATUS Status;
    BOOL b;
    LIST_ENTRY hardlinks, *le;
    ULONG bytes_needed, num_entries = 0;
    FILE_LINK_ENTRY_INFORMATION* feli;
    
    if (fcb->ads)
        return STATUS_INVALID_PARAMETER;
    
    if (*length < offsetof(FILE_LINKS_INFORMATION, Entry))
        return STATUS_INVALID_PARAMETER;
    
    RtlZeroMemory(fli, *length);
    
    InitializeListHead(&hardlinks);
    
    searchkey.obj_id = fcb->inode;
    searchkey.obj_type = TYPE_INODE_REF;
    searchkey.offset = 0;
    
    Status = find_item(fcb->Vcb, fcb->subvol, &tp, &searchkey, FALSE);
    if (!NT_SUCCESS(Status)) {
        ERR("error - find_item returned %08x\n", Status);
        return Status;
    }
    
    bytes_needed = offsetof(FILE_LINKS_INFORMATION, Entry);
    
    do {
        if (tp.item->key.obj_id == fcb->inode) {
            if (tp.item->key.obj_type == TYPE_INODE_REF) {
                ULONG len = tp.item->size;
                INODE_REF* ir = (INODE_REF*)tp.item->data;
                
                if (tp.item->size >= sizeof(INODE_REF)) {
                    UNICODE_STRING dirpath;
                    
                    Status = get_inode_dir_path(fcb->Vcb, fcb->subvol, tp.item->key.offset, &dirpath);
                    if (!NT_SUCCESS(Status)) {
                        ERR("get_inode_dir_path returned %08x\n", Status);
                        goto end;
                    }
                    
                    if (fcb->inode == fcb->subvol->root_item.objid) {
                        name_bit* nb;
                        
                        nb = ExAllocatePoolWithTag(PagedPool, sizeof(name_bit), ALLOC_TAG);
                        if (!nb) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            goto end;
                        }
                        
                        nb->inode = tp.item->key.offset;
                        nb->name = dirpath;
                        
                        InsertTailList(&hardlinks, &nb->list_entry);
                    } else {
                        while (len >= sizeof(INODE_REF) && len >= sizeof(INODE_REF) - 1 + ir->n) {
                            name_bit* nb;
                            ULONG namelen;
                            
                            Status = RtlUTF8ToUnicodeN(NULL, 0, &namelen, ir->name, ir->n);
                            if (!NT_SUCCESS(Status)) {
                                ERR("RtlUTF8ToUnicodeN returned %08x\n", Status);
                                goto end;
                            }
                            
                            if (namelen == 0) {
                                ERR("length was 0\n");
                                Status = STATUS_INTERNAL_ERROR;
                                goto end;
                            }
                            
                            nb = ExAllocatePoolWithTag(PagedPool, sizeof(name_bit), ALLOC_TAG);
                            if (!nb) {
                                ERR("out of memory\n");
                                Status = STATUS_INSUFFICIENT_RESOURCES;
                                goto end;
                            }
                            
                            nb->inode = tp.item->key.offset;
                            nb->name.Buffer = NULL;
                            
                            InsertTailList(&hardlinks, &nb->list_entry);
                            
                            nb->name.Length = nb->name.MaximumLength = namelen + dirpath.Length;
                            
                            nb->name.Buffer = ExAllocatePoolWithTag(PagedPool, nb->name.Length, ALLOC_TAG);
                            if (!nb->name.Buffer) {  
                                ERR("out of memory\n");
                                
                                Status = STATUS_INSUFFICIENT_RESOURCES;
                                goto end;
                            }
                            
                            RtlCopyMemory(nb->name.Buffer, dirpath.Buffer, dirpath.Length);

                            Status = RtlUTF8ToUnicodeN(&nb->name.Buffer[dirpath.Length / sizeof(WCHAR)], namelen, &namelen, ir->name, ir->n);
                            if (!NT_SUCCESS(Status)) {
                                ERR("RtlUTF8ToUnicodeN returned %08x\n", Status);
                                goto end;
                            }
                            
                            ERR("nb->name = %.*S\n", nb->name.Length / sizeof(WCHAR), nb->name.Buffer);
                            
                            if (num_entries > 0)
                                bytes_needed = sector_align(bytes_needed, 8);
                            
                            num_entries++;
                            bytes_needed += sizeof(FILE_LINK_ENTRY_INFORMATION) - sizeof(WCHAR) + max(sizeof(WCHAR), nb->name.Length);
                            
                            len -= sizeof(INODE_REF) - 1 + ir->n;
                            ir = (INODE_REF*)&ir->name[ir->n];
                        }
                        
                        ExFreePool(dirpath.Buffer);
                    }
                }
            } else if (tp.item->key.obj_type == TYPE_INODE_EXTREF) {
                // FIXME
            }
        }
        
        b = find_next_item(fcb->Vcb, &tp, &next_tp, FALSE);
        if (b) {
            tp = next_tp;
            
            if (next_tp.item->key.obj_id > fcb->inode || next_tp.item->key.obj_type > TYPE_INODE_EXTREF)
                break;
        }
    } while (b);
    
    bytes_needed = sector_align(bytes_needed, 8);
    
    fli->BytesNeeded = bytes_needed;
    fli->EntriesReturned = 0;
    
    *length -= offsetof(FILE_LINKS_INFORMATION, Entry);
    feli = NULL;
    
    le = hardlinks.Flink;
    while (le != &hardlinks) {
        name_bit* nb = CONTAINING_RECORD(le, name_bit, list_entry);
        
        if (sector_align(sizeof(FILE_LINK_ENTRY_INFORMATION) - sizeof(WCHAR) + max(sizeof(WCHAR), nb->name.Length), 8) > *length) {
            Status = STATUS_BUFFER_OVERFLOW;
            goto end;
        }
        
        if (feli) {
            feli->NextEntryOffset = sector_align(sizeof(FILE_LINK_ENTRY_INFORMATION) + ((feli->FileNameLength - 1) * sizeof(WCHAR)), 8);
            feli = (FILE_LINK_ENTRY_INFORMATION*)((UINT8*)feli + feli->NextEntryOffset);
        } else
            feli = &fli->Entry;
        
        feli->NextEntryOffset = 0;
        feli->ParentFileId = nb->inode;
        feli->FileNameLength = nb->name.Length / sizeof(WCHAR);
        RtlCopyMemory(feli->FileName, nb->name.Buffer, nb->name.Length);
        
        *length -= sector_align(sizeof(FILE_LINK_ENTRY_INFORMATION) - sizeof(WCHAR) + max(sizeof(WCHAR), nb->name.Length), 8);
        fli->EntriesReturned++;
        
        le = le->Flink;
    }
    
    Status = STATUS_SUCCESS;
    
end:
    while (!IsListEmpty(&hardlinks)) {
        name_bit* nb = CONTAINING_RECORD(hardlinks.Flink, name_bit, list_entry);
        
        if (nb->name.Buffer)
            ExFreePool(nb->name.Buffer);
        
        RemoveEntryList(&nb->list_entry);
        
        ExFreePool(nb);
    }
    return Status;
}

static NTSTATUS STDCALL query_info(device_extension* Vcb, PFILE_OBJECT FileObject, PIRP Irp) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    LONG length = IrpSp->Parameters.QueryFile.Length;
    fcb* fcb = FileObject->FsContext;
    ccb* ccb = FileObject->FsContext2;
    file_ref* fileref = ccb ? ccb->fileref : NULL;
    NTSTATUS Status;
    
    TRACE("(%p, %p, %p)\n", Vcb, FileObject, Irp);
    TRACE("fcb = %p\n", fcb);
    
    if (fcb == Vcb->volume_fcb)
        return STATUS_INVALID_PARAMETER;
    
    switch (IrpSp->Parameters.QueryFile.FileInformationClass) {
        case FileAllInformation:
        {
            FILE_ALL_INFORMATION* fai = Irp->AssociatedIrp.SystemBuffer;
            INODE_ITEM* ii;
            
            TRACE("FileAllInformation\n");
            
            if (fcb->ads) {
                if (!fileref || !fileref->parent) {
                    ERR("no fileref for stream\n");
                    Status = STATUS_INTERNAL_ERROR;
                    goto exit;
                }
                
                ii = &fileref->parent->fcb->inode_item;
            } else
                ii = &fcb->inode_item;
            
            if (length > 0)
                fill_in_file_basic_information(&fai->BasicInformation, ii, &length, fcb, fileref);
            
            if (length > 0)
                fill_in_file_standard_information(&fai->StandardInformation, fcb, fileref, &length);
            
            if (length > 0)
                fill_in_file_internal_information(&fai->InternalInformation, fcb->inode, &length);
            
            if (length > 0)
                fill_in_file_ea_information(&fai->EaInformation, &length);
            
            if (length > 0)
                fill_in_file_access_information(&fai->AccessInformation, &length);
            
            if (length > 0)
                fill_in_file_position_information(&fai->PositionInformation, FileObject, &length);
                
            if (length > 0)
                fill_in_file_mode_information(&fai->ModeInformation, ccb, &length);
            
            if (length > 0)
                fill_in_file_alignment_information(&fai->AlignmentInformation, Vcb, &length);
            
            if (length > 0)
                fill_in_file_name_information(&fai->NameInformation, fcb, fileref, &length);
            
            Status = STATUS_SUCCESS;

            break;
        }

        case FileAttributeTagInformation:
        {
            FILE_ATTRIBUTE_TAG_INFORMATION* ati = Irp->AssociatedIrp.SystemBuffer;
            
            TRACE("FileAttributeTagInformation\n");
            
            Status = fill_in_file_attribute_information(ati, fcb, fileref, &length);
            
            break;
        }

        case FileBasicInformation:
        {
            FILE_BASIC_INFORMATION* fbi = Irp->AssociatedIrp.SystemBuffer;
            INODE_ITEM* ii;
            
            TRACE("FileBasicInformation\n");
            
            if (IrpSp->Parameters.QueryFile.Length < sizeof(FILE_BASIC_INFORMATION)) {
                WARN("overflow\n");
                Status = STATUS_BUFFER_OVERFLOW;
                goto exit;
            }
            
            if (fcb->ads) {
                if (!fileref || !fileref->parent) {
                    ERR("no fileref for stream\n");
                    Status = STATUS_INTERNAL_ERROR;
                    goto exit;
                }
                
                ii = &fileref->parent->fcb->inode_item;
            } else
                ii = &fcb->inode_item;
            
            Status = fill_in_file_basic_information(fbi, ii, &length, fcb, fileref);
            break;
        }

        case FileCompressionInformation:
            FIXME("STUB: FileCompressionInformation\n");
            Status = STATUS_INVALID_PARAMETER;
            goto exit;

        case FileEaInformation:
        {
            FILE_EA_INFORMATION* eai = Irp->AssociatedIrp.SystemBuffer;
            
            TRACE("FileEaInformation\n");
            
            Status = fill_in_file_ea_information(eai, &length);
            
            break;
        }

        case FileInternalInformation:
        {
            FILE_INTERNAL_INFORMATION* fii = Irp->AssociatedIrp.SystemBuffer;
            
            TRACE("FileInternalInformation\n");
            
            Status = fill_in_file_internal_information(fii, fcb->inode, &length);
            
            break;
        }

        case FileNameInformation:
        {
            FILE_NAME_INFORMATION* fni = Irp->AssociatedIrp.SystemBuffer;
            
            TRACE("FileNameInformation\n");
            
            Status = fill_in_file_name_information(fni, fcb, fileref, &length);
            
            break;
        }

        case FileNetworkOpenInformation:
        {
            FILE_NETWORK_OPEN_INFORMATION* fnoi = Irp->AssociatedIrp.SystemBuffer;
            
            TRACE("FileNetworkOpenInformation\n");
            
            Status = fill_in_file_network_open_information(fnoi, fcb, fileref, &length);

            break;
        }

        case FilePositionInformation:
        {
            FILE_POSITION_INFORMATION* fpi = Irp->AssociatedIrp.SystemBuffer;
            
            TRACE("FilePositionInformation\n");
            
            Status = fill_in_file_position_information(fpi, FileObject, &length);
            
            break;
        }

        case FileStandardInformation:
        {
            FILE_STANDARD_INFORMATION* fsi = Irp->AssociatedIrp.SystemBuffer;
            
            TRACE("FileStandardInformation\n");
            
            if (IrpSp->Parameters.QueryFile.Length < sizeof(FILE_STANDARD_INFORMATION)) {
                WARN("overflow\n");
                Status = STATUS_BUFFER_OVERFLOW;
                goto exit;
            }
            
            Status = fill_in_file_standard_information(fsi, fcb, ccb->fileref, &length);
            
            break;
        }

        case FileStreamInformation:
        {
            FILE_STREAM_INFORMATION* fsi = Irp->AssociatedIrp.SystemBuffer;
            
            TRACE("FileStreamInformation\n");
            
            Status = fill_in_file_stream_information(fsi, fcb, &length);

            break;
        }

        case FileHardLinkInformation:
        {
            FILE_LINKS_INFORMATION* fli = Irp->AssociatedIrp.SystemBuffer;
            
            TRACE("FileHardLinkInformation\n");
            
            Status = fill_in_hard_link_information(fli, fcb, &length);
            
            break;
        }
            
        case FileNormalizedNameInformation:
        {
            FILE_NAME_INFORMATION* fni = Irp->AssociatedIrp.SystemBuffer;
            
            TRACE("FileNormalizedNameInformation\n");
            
            Status = fill_in_file_name_information(fni, fcb, fileref, &length);
            
            break;
        }
            
        case FileStandardLinkInformation:
        {
            FILE_STANDARD_LINK_INFORMATION* fsli = Irp->AssociatedIrp.SystemBuffer;
            
            TRACE("FileStandardLinkInformation\n");
            
            Status = fill_in_file_standard_link_information(fsli, fcb, ccb->fileref, &length);
            
            break;
        }
        
        case FileRemoteProtocolInformation:
            TRACE("FileRemoteProtocolInformation\n");
            Status = STATUS_INVALID_PARAMETER;
            goto exit;
        
        default:
            WARN("unknown FileInformationClass %u\n", IrpSp->Parameters.QueryFile.FileInformationClass);
            Status = STATUS_INVALID_PARAMETER;
            goto exit;
    }
    
    if (length < 0) {
        length = 0;
        Status = STATUS_BUFFER_OVERFLOW;
    }
    
    Irp->IoStatus.Information = IrpSp->Parameters.QueryFile.Length - length;

exit:    
    TRACE("query_info returning %08x\n", Status);
    
    return Status;
}

NTSTATUS STDCALL drv_query_information(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp) {
    PIO_STACK_LOCATION IrpSp;
    NTSTATUS Status;
    fcb* fcb;
    device_extension* Vcb = DeviceObject->DeviceExtension;
    BOOL top_level;
    
    FsRtlEnterFileSystem();
    
    top_level = is_top_level(Irp);
    
    if (Vcb && Vcb->type == VCB_TYPE_PARTITION0) {
        Status = part0_passthrough(DeviceObject, Irp);
        goto exit;
    }
    
    Irp->IoStatus.Information = 0;
    
    TRACE("query information\n");
    
    IrpSp = IoGetCurrentIrpStackLocation(Irp);
    
    ExAcquireResourceSharedLite(&Vcb->tree_lock, TRUE);
    
    fcb = IrpSp->FileObject->FsContext;
    TRACE("fcb = %p\n", fcb);
    TRACE("fcb->subvol = %p\n", fcb->subvol);
    
    Status = query_info(fcb->Vcb, IrpSp->FileObject, Irp);
    
    TRACE("returning %08x\n", Status);
    
    Irp->IoStatus.Status = Status;
    
    IoCompleteRequest( Irp, IO_NO_INCREMENT );
    
    ExReleaseResourceLite(&Vcb->tree_lock);
    
exit:
    if (top_level) 
        IoSetTopLevelIrp(NULL);
    
    FsRtlExitFileSystem();
    
    return Status;
}

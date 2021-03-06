#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/tree.h>

#define VERSION "10"
#define nelem(A) (sizeof (A) / sizeof (A)[0])
#define DEV_PATH_LEN 256
#define TAG_LEN 32

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jesper Madsen <jmad@itu.dk>");
MODULE_DESCRIPTION("AoE Test Driver, exposing a SysFS interface to test modified AoE driver");
MODULE_VERSION(VERSION);

static struct kmem_cache *tree_iface_pool = NULL;
static struct bio_set *bio_pool = NULL;

static u8 empty_read_buffer = 255;

static void aoetest_release(struct kobject *kobj);
static struct kobj_type aoetest_ktype_device;

struct aoedev {
    struct kobject kobj;
    /*ref to blockdevice*/
    struct block_device *blkdev;
    /*path to device*/
    char dev_path[DEV_PATH_LEN];
    /*tag to identify device by*/
    char tag[TAG_LEN];
    /*next device*/
    struct aoedev *next;
};

/*kobject for the module itself*/
static struct kobject aoetest_kobj;
/*points to head the aoedev list, one entry per aoedev added*/
static struct aoedev *devlist = NULL;

/*to enforce order when adding/modifying aoedev refs*/
static spinlock_t lock;

/*shorthand container for sysfs entries*/
struct aoetest_sysfs_entry {
    struct attribute attr;
    ssize_t (*show)(struct aoedev *, char *);
	ssize_t (*store)(struct aoedev *, const char *, size_t);
};

enum BIO_TYPE{
    ATA_BIO,
    TREE_BIO,
};

struct submit_syncbio_data {
	struct completion event;
	int error;
};

static __always_inline void log_cmd(char *cmd_name)
{
    printk("AoETest, CMD SENT: %s\n", cmd_name);
}

static __always_inline int is_tree_bio(struct bio *b)
{
    return (b->bi_treecmd != NULL);
}

static __always_inline void cleanup_if_treebio(struct bio *b)
{
    /*all done, cleanup time*/
    if (is_tree_bio(b)) { 
        printk("cleanup_if_treebio: is_tree_bio(b) => true\n");
        kmem_cache_free(tree_iface_pool, b->bi_treecmd);
        b->bi_private = NULL;
    }
}

/**
 * Called when a sync bio is finished.
 * @description a function which populates some fields in
 *              preparation for the end of a synchronous bio.
 * @param b the finished bio which was intended to be
 *          synchronous
 * @param error error code of bio, 0 if no error occurred.
 */
void submit_bio_syncio(struct bio *b, int error)
{
	struct submit_syncbio_data *ret = b->bi_private;

	ret->error = error;
	complete(&ret->event);

    /*FIXME this is kind of stupid, just let the user manually clean up 
      by first issuing get_bio(b), then calling cleanup_if_treebio himself*/
    cleanup_if_treebio(b); 
}

/**
 * Called when a bio is finished.
 * @description bio's allocated via the alloc_bio helper method 
 *              will have this function called when they are
 *              finished.
 * @param b the finished bio 
 * @param error error code of bio, 0 if no error occurred.
 */
void alloc_bio_end_fnc(struct bio *b, int error)
{
    printk("alloc_bio_end_fnc run\n");
    /*FIXME this is kind of stupid, just let the user manually clean up 
      by first issuing get_bio(b), then calling cleanup_if_treebio himself*/
    cleanup_if_treebio(b);
    
    if(b->bi_private)
        kfree(b->bi_private); 
}

/** 
 * Allocate bios for both ATA and TREE commands. 
 * @param bt the type of BIO desired, ATA_BIO or TREE_BIO
 * @return NULL on allocation error, otherwise a bio. If a TREE 
 *         bio, the bi_treecmd field points to a struct
 *         tree_iface_data.
 * @note it is your responsibility to call bio_put() once the 
 *       bio is submitted and you're otherwise done with it.
 * @note to issue a valid tree cmd, b->bi_treecmd needs 
 *       additional data.
 */ 
struct bio *alloc_bio(enum BIO_TYPE bt)
{
    struct bio *b = NULL;
    b = bio_alloc_bioset(GFP_ATOMIC, 1, bio_pool);
    if (!b)
        goto err_alloc_bio;

    bio_get(b); /*ensure bio won't disappear on us*/

    if (bt == TREE_BIO) {
        struct tree_iface_data *td = NULL;
        td = kmem_cache_alloc(tree_iface_pool, GFP_ATOMIC);
        if (!td)
            goto err_alloc_tree_iface;
        memset(td, 0, sizeof *td);

        b->bi_treecmd = td;

        b->bi_sector = 0; /*using td->off to mark offsets for read/write*/
        
    } else {
        b->bi_treecmd = NULL;
    }

    b->bi_end_io = &alloc_bio_end_fnc;

    return b;

err_alloc_tree_iface:
    bio_put(b);
err_alloc_bio:
    return NULL;
}

/**
 * Helper method for deallocating bio's.
 * @param b the bio to release
 * @note only call this if you're sure releasing one more
 *       reference will actually dealloc the bio.
 */ 
void dealloc_bio(struct bio *b)
{
    BUG_ON( atomic_read(&b->bi_cnt) != 1 );

    if (b->bi_treecmd) { /*assume tree bio*/
        kmem_cache_free(tree_iface_pool, b->bi_treecmd);
        b->bi_treecmd = NULL;
    }

    bio_put(b);
}

/**
 * Submit a bio and wait for its completion. 
 * @description wraps submit_bio into a synchronous call, using only the bi_private field. 
 * @param bio the bio to wait for 
 * @param rw read/write flag, accepts REQ_* values & WRITE
 * @return the error code of the completed bio                 
 */ 
int submit_bio_sync(struct bio *bio, int rw)
{
    struct submit_syncbio_data ret;

	rw |= REQ_SYNC;
	/*initialise queue*/
    ret.event.done = 0;
    init_waitqueue_head(&ret.event.wait);

	bio->bi_private = &ret;
	bio->bi_end_io = submit_bio_syncio;
	submit_bio(rw, bio);
	wait_for_completion(&ret.event);

	return ret.error;
}



/** 
 * @param p the raw argument string 
 * @param argv an array of character pointers. To hold the start 
 *             of a word each (word: a series of characters
 * @param argv_max maximum number of individual arguments (the 
 *                 number of character pointers in argv)
 * @return the number of individual arguments parsed from the 
 *         string, stored in argv[0] and up
*/ 
static ssize_t aoetest_sysfs_args(char *p, char *argv[], int argv_max)
{
	int argc = 0;

	while (*p) {
		while (*p && isspace(*p))
			++p;
		if (*p == '\0')
			break;
		if (argc < argv_max)
			argv[argc++] = p;
		else {
			printk(KERN_ERR "too many args!\n");
			return -1;
		}
		while (*p && !isspace(*p))
			++p;
		if (*p)
			*p++ = '\0';
	}
	return argc;
}

/** 
 * split arg string into words, storing each pointer in argv. 
 * @param page the input string to be parsed and split into 
 *             words
 * @param len the length of the input string 
 * @param p will hold the memory allocation made by this
 *          function.
 * @param argv an array of char*, will have a non-null entry for 
 *             each word found in the input string given by
 *             'page'
 * @param argv_max maximum entries that argv can hold. 
 * @pre '*p' is NULL 
 * @pre 'page' points to the start of a c-string 
 * @pre 'argv' is an allocated array of char* of at least as 
 *      great a size as the number of arguments expected
 * @note you are responsible for releasing the memory pointed to 
 *       by *p using kfree, provided the function returns
 *       successfully
 * @return -ENOMEM if allocation fails, otherwise the number of 
 *         words found.
 */ 
static ssize_t __parse_args(const char *page, size_t len, char **p, char *argv[], int argv_max)
{
	*p = kmalloc(len+1, GFP_ATOMIC);
    if (*p == NULL) {
        printk(KERN_ERR "aoedev_store_add: could not allocate memory for string buffer\n");
        return -ENOMEM;
    }
	memcpy(*p, page, len);
	(*p)[len] = '\0';
	
    return aoetest_sysfs_args(*p, argv, argv_max);
}

static ssize_t __aoedev_add_dev(char *dev_path , char *tag)
{
	struct block_device *bd;
	struct aoedev *d, *curr_d;
    int ret = 0;

	printk("__aoedev_add_dev\n");

	bd = blkdev_get_by_path(dev_path, FMODE_READ|FMODE_WRITE, NULL);
	if (!bd || IS_ERR(bd)) {
		printk(KERN_ERR "add failed: can't open block device %s: %ld\n", dev_path, PTR_ERR(bd));
		return -ENOENT;
	}
    printk("found device\n");

	if (get_capacity(bd->bd_disk) == 0) {
		printk(KERN_ERR "add failed: zero sized block device.\n");
		ret = -ENOENT;
		goto err_dev;
	}
    printk("checked capacity\n");

	spin_lock(&lock);
    printk("locked\n");
    /*Guard against adding the same device multiple times*/
    for (curr_d = devlist; curr_d; curr_d = curr_d->next) {
        if( strncmp(dev_path, curr_d->dev_path, DEV_PATH_LEN) == 0) {
            printk("err out\n");
            spin_unlock(&lock);
            printk(KERN_ERR "device already added to AoE Test module (%s)\n", dev_path);
            ret = -EEXIST;
            goto err_dev;
        }
    }
    printk("malloc GFP_ATOMIC\n");
	d = kmalloc(sizeof(struct aoedev), GFP_ATOMIC);
	if (!d) {
		printk(KERN_ERR "add failed: kmalloc error for '%s'\n", dev_path);
		ret = -ENOMEM;
		goto err_alloc_dev;
	}
    printk("memset\n");
	memset(d, 0, sizeof(struct aoedev));
	d->blkdev = bd;
	strncpy(d->dev_path, dev_path, nelem(d->dev_path)-1);
    strncpy(d->tag, tag, nelem(d->tag)-1);
	printk("kobject_init_and_add\n");

	if (kobject_init_and_add(&d->kobj, &aoetest_ktype_device, &aoetest_kobj, "%s", tag)){
        ret = -1; /*honestly no idea what went wrong*/
        goto err_add_kobject;
    }
    printk("init and unlock\n");
    /*prepend dev to devlist*/
	d->next = devlist;
	devlist = d;
	spin_unlock(&lock);

	printk("Exposed TREE/ATA interface of device '%s', tagged: '%s'\n", d->dev_path, d->tag);
	return 0;
err_add_kobject:
    kfree(d);
err_alloc_dev:
err_dev:
    blkdev_put(bd, FMODE_READ|FMODE_WRITE);
	return ret;
}

static ssize_t aoedev_store_add(struct aoedev *dev, const char *page, size_t len)
{
	int error = 0;
	char *argv[16];
	char *p = NULL;
    int numargs = __parse_args(page,len,&p,argv,nelem(argv));

    if (unlikely(numargs < 0)) {
        goto parse_args_err;
    } else if (numargs != 2) {
        printk(KERN_ERR "bad arg count for add\n");
        error = -EINVAL;
    } else {
        error = __aoedev_add_dev(argv[0], argv[1]); /*dev_path, tag*/
    }

	kfree(p);
	return error ? error : len;
parse_args_err:
    return -EINVAL;
}
static struct aoetest_sysfs_entry aoedev_sysfs_add = __ATTR(add, 0644, NULL, aoedev_store_add);

static ssize_t __aodev_del_dev(char *tag)
{
	struct aoedev *d, **b;
	int ret;

	b = &devlist;
	d = devlist;
	spin_lock(&lock);
	
	for (; d; b = &d->next, d = *b) {
        if (strncmp(tag, d->tag, TAG_LEN) == 0) {
            break;
        }
    }

	if (d == NULL) {
		printk(KERN_ERR "del failed: no dekmem_tree_iface_cachevice by tag %s not found.\n", 
			tag);
		ret = -ENOENT;
		goto err;
	}

	*b = d->next;
	
	spin_unlock(&lock);
	
	blkdev_put(d->blkdev, FMODE_READ|FMODE_WRITE);
	
	kobject_del(&d->kobj);
	kobject_put(&d->kobj);
	
	return 0;
err:
	spin_unlock(&lock);
	return ret;
}

static ssize_t aoedev_store_del(struct aoedev *dev, const char *page, size_t len)
{
	int error = 0;
	char *argv[16];
	char *p = NULL;
    int numargs = __parse_args(page,len,&p,argv,nelem(argv));

    if (numargs < 0)
    {
        printk(KERN_ERR "failed to allocate buffer for parsing input\n");
        return -EINVAL;
    } else if (numargs != 1) {
        printk(KERN_ERR "expects 1 argument only\n");
        error = -EINVAL;
    }
    else {
        __aodev_del_dev(argv[0]);
    }

	kfree(p);
	return error ? error : len;
}
static struct aoetest_sysfs_entry aoedev_sysfs_del = __ATTR(del, 0644, NULL, aoedev_store_del);

/*module-level attrs*/
static struct attribute *aoetest_ktype_module_attrs[] = {
	&aoedev_sysfs_add.attr,
	&aoedev_sysfs_del.attr,
	NULL,
};

static ssize_t show_devpath(struct aoedev *dev, char *page)
{
    return sprintf(page, "%s\n", dev->dev_path);
}

/*
static ssize_t store_model(struct aoedev *dev, const char *page, size_t len)
{
	spncpy(dev->model, page, nelem(dev->model));
	return 0;
} 
*/
static struct aoetest_sysfs_entry aoetest_sysfs_devpath = __ATTR(tag, 0644, show_devpath, NULL);
 
static ssize_t store_createtree(struct aoedev *dev, const char *page, size_t len)
{
    struct bio *b = NULL;
    struct tree_iface_data *td = NULL;
    struct page *p;
    ulong bcnt, vec_off;

    printk("store_createtree called\n");

    b = alloc_bio(TREE_BIO);
    if (!b)
        goto err_alloc;

    td = (struct tree_iface_data *) b->bi_treecmd;

    td->cmd = AOECMD_CREATETREE;
    td->tid = 0; /*ignored now, set on return*/

    b->bi_bdev = dev->blkdev;
    p = virt_to_page(&empty_read_buffer);
    bcnt = sizeof(empty_read_buffer);
    vec_off = offset_in_page(&empty_read_buffer);

    if (bio_add_page(b,p,bcnt,vec_off) < bcnt) {
        goto err_page_add;
    }
    submit_bio(READ, b);
    log_cmd("create_tree");

    return len;
err_page_add:
    dealloc_bio(b);
err_alloc:
    printk("create_tree sysfs call failed\n");
    return len;
}
static struct aoetest_sysfs_entry aoetest_sysfs_createtree = __ATTR(create_tree, 0644, NULL, store_createtree);

static ssize_t store_removetree(struct aoedev *dev, const char *page, size_t len)
{
    /*arg parse stuff*/
    char *pstr = NULL;
    char *argv[16];
    int numargs;

    /*call-specific*/
    struct bio *b = NULL;
    struct tree_iface_data *td = NULL;
    struct page *p;
    ulong bcnt, vec_off;
    size_t error;

    printk("store_removetree called\n");
    numargs = __parse_args(page,len,&pstr,argv,nelem(argv));
    if (numargs < 0) {
        printk(KERN_ERR "failed to allocate buffer for parsing input string\n");
        error = -ENOMEM;
        goto err;
    } else if (numargs != 1) {
        printk(KERN_ERR "expected 1 argument, the tree identifier\n");
        error = -EINVAL;
        goto err_arg_input;
    }

    b = alloc_bio(TREE_BIO);
    if (!b) {
        error = -ENOMEM;
        goto err_bio_alloc;
    }

    td = (struct tree_iface_data *) b->bi_treecmd;

    td->cmd = AOECMD_REMOVETREE;
    if (kstrtou64(argv[0],10,&td->tid)) {
        error = -EINVAL;
        goto err_numparse;
    }

    b->bi_bdev = dev->blkdev;
    p = virt_to_page(&empty_read_buffer);
    bcnt = sizeof(empty_read_buffer);
    vec_off = offset_in_page(&empty_read_buffer);

    if (bio_add_page(b,p,bcnt,vec_off) < bcnt) {
        error = -ENOMEM;
        goto err_page_add;
    }

    submit_bio(READ, b);
    log_cmd("remove_tree");

    return len;
err_page_add:
err_numparse:
    dealloc_bio(b);
err_bio_alloc:
err_arg_input:
    kfree(pstr);
err:
    printk(KERN_ERR "remove_tree sysfs call failed\n");
    return error;
}
static struct aoetest_sysfs_entry aoetest_sysfs_removetree = __ATTR(remove_tree, 0644, NULL, store_removetree);

static ssize_t store_insertnode(struct aoedev *dev, const char *page, size_t len)
{
    /*arg parse stuff*/
    char *pstr = NULL;
    char *argv[16];
    int numargs;
    /*call-specific*/
    struct bio *b = NULL;
    struct tree_iface_data *td = NULL;
    struct page *p;
    ulong bcnt, vec_off;
    ssize_t error;

    printk("store_insertnode called\n");
    numargs = __parse_args(page,len,&pstr,argv,nelem(argv));
    if (numargs < 0) {
        printk(KERN_ERR "failed to allocate buffer for parsing input string\n");
        error = -ENOMEM;
        goto err;
    } else if (numargs != 1) {
        printk(KERN_ERR "expected 1 argument, the tree identifier\n");
        error = -EINVAL;
        goto err_arg_input;
    }

    b = alloc_bio(TREE_BIO);
    if (!b) {
        printk("aoetest - insertnode: failed to allocate a bio\n");
        error = -ENOMEM;
        goto err_bio_alloc;
    }

    td = (struct tree_iface_data *) b->bi_treecmd;
    
    td->cmd = AOECMD_INSERTNODE;
    if (kstrtou64(argv[0],10,&td->tid)) {
        error = -EINVAL;
        goto err_numparse;
    }
    
    b->bi_bdev = dev->blkdev;
    b->bi_end_io = alloc_bio_end_fnc;

    p = virt_to_page(&empty_read_buffer);
    bcnt = sizeof(empty_read_buffer);
    vec_off = offset_in_page(&empty_read_buffer);

    printk("b4 bio_add_page\n");
    if (bio_add_page(b, p, bcnt, vec_off) < bcnt) {
        printk(KERN_ERR "insert_node bio could not add page worth %lu bytes of data\n", bcnt);
        error = -ENOMEM;
        goto err_page_add;
    }

    submit_bio(WRITE, b);
    log_cmd("insert_node");
    return len;

err_page_add:
err_numparse:
    dealloc_bio(b);
err_bio_alloc:
err_arg_input:
    kfree(pstr);
err:
    printk(KERN_ERR "insert_node sysfs call failed\n");
    return error;
}
static struct aoetest_sysfs_entry aoetest_sysfs_insertnode = __ATTR(insert_node, 0644, NULL, store_insertnode);

static ssize_t store_updatenode(struct aoedev *dev, const char *page, size_t len)
{
    /*arg parse stuff*/
    char *pstr = NULL;
    char *argv[16];
    int numargs;
    /*call-specific*/
    struct bio *b = NULL;
    struct tree_iface_data *td = NULL;
    struct page *p;
    ulong bcnt, vec_off;
    ssize_t error;

    printk("store_updatenode called\n");
    numargs = __parse_args(page,len,&pstr,argv,nelem(argv));
    if (numargs < 0) {
        printk(KERN_ERR "failed to allocate buffer for parsing input string\n");
        error = -ENOMEM;
        goto err;
    } else if (numargs != 5) { /*ARGS: tid nid off len data*/
        printk(KERN_ERR "expected 5 arguments (tid,nid,off,len,data)\n");
        error = -EINVAL;
        goto err_arg_input;
    }

    b = alloc_bio(TREE_BIO);
    if (!b) {
        printk("aoetest - updatenode: failed to allocate a bio\n");
        error = -ENOMEM;
        goto err_bio_alloc;
    }

    td = (struct tree_iface_data *) b->bi_treecmd;
    
    td->cmd = AOECMD_UPDATENODE;
    if (kstrtou64(argv[0],10,&td->tid)) {
        error = -EINVAL;
        goto err_numparse;
    }
    if (kstrtou64(argv[1],10,&td->nid)) {
        error = -EINVAL;
        goto err_numparse;
    }
    if (kstrtou64(argv[2],10,&td->off)) {
        error = -EINVAL;
        goto err_numparse;
    }
    if (kstrtou64(argv[3],10,&td->len)) {
        error = -EINVAL;
        goto err_numparse;
    }
    
    b->bi_bdev = dev->blkdev;
    b->bi_end_io = alloc_bio_end_fnc;

    p = virt_to_page(argv[4]);
    bcnt = (ulong)(td->len & 0xFFFFFFFF); /*test code, that's why this is acceptable*/
    vec_off = offset_in_page(argv[4]);

    printk("b4 bio_add_page (argv[4]: %s)\n", argv[4]);
    if (bio_add_page(b, p, bcnt, vec_off) < bcnt) {
        printk(KERN_ERR "update_node bio could not add page worth %lu bytes of data\n", bcnt);
        error = -ENOMEM;
        goto err_page_add;
    }

    submit_bio(WRITE, b);
    log_cmd("update_node");
    return len;

err_page_add:
err_numparse:
    dealloc_bio(b);
err_bio_alloc:
err_arg_input:
    kfree(pstr);
err:
    printk(KERN_ERR "update_node sysfs call failed\n");
    return error;
}
static struct aoetest_sysfs_entry aoetest_sysfs_updatenode = __ATTR(update_node, 0644, NULL, store_updatenode);

static ssize_t store_removenode(struct aoedev *dev, const char *page, size_t len)
{
    /*arg parse stuff*/
    char *pstr = NULL;
    char *argv[16];
    int numargs;
    /*call-specific*/
    struct bio *b = NULL;
    struct tree_iface_data *td = NULL;
    struct page *p;
    ulong bcnt, vec_off;
    ssize_t error;

    printk("store_removenode called\n");
    numargs = __parse_args(page,len,&pstr,argv,nelem(argv));
    if (numargs < 0) {
        printk(KERN_ERR "failed to allocate buffer for parsing input string\n");
        error = -ENOMEM;
        goto err;
    } else if (numargs != 2) {
        printk(KERN_ERR "expected 2 arguments: tid nid\n");
        error = -EINVAL;
        goto err_arg_input;
    }

    b = alloc_bio(TREE_BIO);
    if (!b) {
        printk("aoetest - removenode: failed to allocate a bio\n");
        error = -ENOMEM;
        goto err_bio_alloc;
    }

    td = (struct tree_iface_data *) b->bi_treecmd;
    
    /*I could convert the value straight into the treecmd 
      struct but I'd get unused result warnings...*/
    td->cmd = AOECMD_REMOVENODE;
    if (kstrtou64(argv[0],10,&td->tid)){
        error = -EINVAL;
        goto err_numparse;
    }
    if (kstrtou64(argv[1],10,&td->nid)){
        error = -EINVAL;
        goto err_numparse;
    }
    
    b->bi_bdev = dev->blkdev;
    b->bi_end_io = alloc_bio_end_fnc;

    p = virt_to_page(&empty_read_buffer);
    bcnt = sizeof(empty_read_buffer);
    vec_off = offset_in_page(&empty_read_buffer);

    printk("b4 bio_add_page\n");
    if (bio_add_page(b, p, bcnt, vec_off) < bcnt) {
        printk(KERN_ERR "remove_node bio could not add page worth %lu bytes of data\n", bcnt);
        error = -ENOMEM;
        goto err_page_add;
    }

    submit_bio(WRITE, b);
    log_cmd("remove_node");
    return len;

err_page_add:
err_numparse:
    dealloc_bio(b);
err_bio_alloc:
err_arg_input:
    kfree(pstr);
err:
    printk(KERN_ERR "remove_node sysfs call failed\n");
    return error;
}
static struct aoetest_sysfs_entry aoetest_sysfs_removenode = __ATTR(remove_node, 0644, NULL, store_removenode);

static ssize_t store_readnode(struct aoedev *dev, const char *page, size_t len)
{
    /*arg parse stuff*/
    char *pstr = NULL;
    char *argv[16];
    int numargs;
    /*call-specific*/
    struct bio *b = NULL;
    struct tree_iface_data *td = NULL;
    struct page *p;
    ulong bcnt, vec_off;
    ssize_t error;
    u8 *buffer = NULL;

    printk("store_readnode called\n");
    numargs = __parse_args(page,len,&pstr,argv,nelem(argv));
    if (numargs < 0) {
        printk(KERN_ERR "failed to allocate buffer for parsing input string\n");
        error = -ENOMEM;
        goto err;
    } else if (numargs != 4) { /*ARGS: tid nid off len*/
        printk(KERN_ERR "expected 4 arguments (tid,nid,off,len)\n");
        error = -EINVAL;
        goto err_arg_input;
    }

    b = alloc_bio(TREE_BIO);
    if (!b) {
        printk("\tfailed to allocate a bio\n");
        error = -ENOMEM;
        goto err_bio_alloc;
    }
    td = (struct tree_iface_data *) b->bi_treecmd;

    
    td->cmd = AOECMD_READNODE;
    if (kstrtou64(argv[0],10,&td->tid)) {
        error = -EINVAL;
        printk(KERN_ERR "\tfailed to convert input to u64\n");
        goto err_numparse;
    }
    if (kstrtou64(argv[1],10,&td->nid)) {
        error = -EINVAL;
        printk(KERN_ERR "\tfailed to convert input to u64\n");
        goto err_numparse;
    }
    if (kstrtou64(argv[2],10,&td->off)) {
        error = -EINVAL;
        printk(KERN_ERR "\tfailed to convert input to u64\n");
        goto err_numparse;
    }
    if (kstrtou64(argv[3],10,&td->len)) {
        error = -EINVAL;
        printk(KERN_ERR "\tfailed to convert input to u64\n");
        goto err_numparse;
    }

    buffer = kmalloc(len, GFP_ATOMIC);
    if (!buffer) {
        error = -ENOMEM;
        printk(KERN_ERR "\tfailed to allocate read buffer\n");
        goto err_buffer_alloc;
    }
    
    b->bi_bdev = dev->blkdev;
    b->bi_end_io = alloc_bio_end_fnc;

    p = virt_to_page(buffer);
    bcnt = (ulong)(td->len & 0xFFFFFFFF);
    vec_off = offset_in_page(buffer);

    printk("b4 bio_add_page\n");
    if (bio_add_page(b, p, bcnt, vec_off) < bcnt) {
        printk(KERN_ERR "read_node bio could not add page worth %lu bytes of data\n", bcnt);
        error = -ENOMEM;
        goto err_page_add;
    }

    submit_bio(WRITE, b);
    log_cmd("read_node");
    return len;

err_page_add:
    kfree(buffer);
err_buffer_alloc:
err_numparse:
    dealloc_bio(b);
err_bio_alloc:
err_arg_input:
    kfree(pstr);
err:
    printk(KERN_ERR "read_node sysfs call failed\n");
    return error;
}
static struct aoetest_sysfs_entry aoetest_sysfs_readnode = __ATTR(read_node, 0644, NULL, store_readnode);

/*device-level attrs*/
static struct attribute *aoetest_ktype_device_attrs[] = {
    &aoetest_sysfs_devpath.attr,
    &aoetest_sysfs_createtree.attr,
    &aoetest_sysfs_removetree.attr,
    &aoetest_sysfs_insertnode.attr,
    &aoetest_sysfs_updatenode.attr,
    &aoetest_sysfs_removenode.attr,
    &aoetest_sysfs_readnode.attr,
    NULL,
};

static ssize_t aoetest_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	struct aoetest_sysfs_entry *entry;
	struct aoedev *dev;

	entry = container_of(attr, struct aoetest_sysfs_entry, attr);
	dev = container_of(kobj, struct aoedev, kobj);

	if (!entry->show)
		return -EIO;

	return entry->show(dev, page);
}

static ssize_t aoetest_attr_store(struct kobject *kobj, struct attribute *attr,
			const char *page, size_t length)
{
	ssize_t ret;
	struct aoetest_sysfs_entry *entry;

	entry = container_of(attr, struct aoetest_sysfs_entry, attr);

	if (kobj == &aoetest_kobj) /*module-level*/
		ret = entry->store(NULL, page, length);
	else { /*device-level*/
		struct aoedev *dev = container_of(kobj, struct aoedev, kobj);

		if (!entry->store)
			return -EIO;

		ret = entry->store(dev, page, length);
	}

	return ret;
}

/*show(read), store(write) functions for module-level and device-level settings alike*/
static const struct sysfs_ops aoetest_sysfs_ops = {
	.show		= aoetest_attr_show,
	.store		= aoetest_attr_store,
};

/*top-level => module-level */
static struct kobj_type aoetest_ktype_module = {
	.default_attrs	= aoetest_ktype_module_attrs,
	.sysfs_ops		= &aoetest_sysfs_ops, /*show/store for module-level interface files*/
	.release		= aoetest_release, /*No-OP*/
};

/*device-level */
static struct kobj_type aoetest_ktype_device = {
	.default_attrs	= aoetest_ktype_device_attrs,
	.sysfs_ops		= &aoetest_sysfs_ops, /*show/store for module-level interface files*/
	.release		= aoetest_release, /*No-OP*/
};

static void aoetest_release(struct kobject *kobj)
{} /*NO-OP*/


static __exit void
aoe_exit(void)
{
    kobject_del(&aoetest_kobj);
	kobject_put(&aoetest_kobj);

    if (tree_iface_pool)
        kmem_cache_destroy(tree_iface_pool);
    if (bio_pool)
        bioset_free(bio_pool);

	return;
}

static __init int
aoe_init(void)
{
    spin_lock_init(&lock);

    if (kobject_init_and_add(&aoetest_kobj, &aoetest_ktype_module, NULL, "aoetest"))
        return -1;
    
    tree_iface_pool = kmem_cache_create("tree_iface_data", sizeof (struct tree_iface_data), 0, 0, NULL);
    if (tree_iface_pool == NULL)
        goto err_alloc_treepool;
    
    bio_pool = bioset_create(100, 0); /*pool_size, front_padding (if using larger structure than a bio)*/
    if (bio_pool == NULL) {
        goto err_alloc_biopool;
    }

    return 0;
err_alloc_biopool:
    kmem_cache_destroy(tree_iface_pool);
err_alloc_treepool:
    return -ENOMEM;
}

module_init(aoe_init);
module_exit(aoe_exit);

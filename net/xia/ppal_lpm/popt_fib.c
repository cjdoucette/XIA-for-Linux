#include <linux/slab.h>
#include <net/xia_dag.h>
#include <net/xia_lpm.h>
#include <linux/rwlock.h>
#include <net/xia.h>
#include <linux/vmalloc.h>


typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;


//#define uint160 uint8_t [20]
#define POPTRIE_S               18
#define POPTRIE_INIT_FIB_SIZE   4096

#define popcnt(v)               __builtin_popcountll(v)




typedef struct uint160_t{
    	uint32_t prefix1;
    	__uint128_t prefix2;
}XID;

/* Internal node */
typedef struct poptrie_node {
    	u64 leafvec;
    	u64 vector;
    	u32 base0;
    	u32 base1;
} poptrie_node_t;

/* Leaf node */
typedef u16 poptrie_leaf_t;

/*
 * Radix tree node for XID's
 */
struct radix_node160 {
    	int valid;
    	struct radix_node160 *left;
    	struct radix_node160 *right;

    	/* Next hop */
    	XID prefix;
    	int len;
    	poptrie_leaf_t nexthop;

    	/* Propagated route */
    	struct radix_node160 *ext;

    	/* Mark for update */
    	int mark;
};

/*
 * FIB mapping table
 */
struct poptrie_fib {
    	void **entries;
    	int n;
    	int sz;
};


struct popt_fib_xid_table {
	/* Root */
    	u32 root;

    	/* FIB */
    	struct poptrie_fib fib;

    	/* Memory management */
    	poptrie_node_t *nodes;
    	poptrie_leaf_t *leaves;
    	void *cnodes;
    	void *cleaves;

    	/* Size */
    	int nodesz;
    	int leafsz;

    	u32 last_base0;
    	u32 last_base1;

    	/* Direct pointing */
    	u32 *dir;
    	u32 *altdir;

    	/* RIB */
    	struct radix_node160 *radix;

    	/* Control */
    	int _allocated;

	/* RCU is currently not used on the tree, so we use a rwlock. */
	rwlock_t		writers_lock;
};

/*
 * Free the allocated memory by the radix tree
 */
static void
_release_radix160(struct radix_node160 *node)
{
    if ( NULL != node  ) {
        _release_radix160(node->left);
        _release_radix160(node->right);
        vfree(node);
    }
}

/*
 * Release the poptrie data structure
 */
void
poptrie160_release(struct popt_fib_xid_table *poptrie)
{
    /* Release the radix tree */
    _release_radix160(poptrie->radix);

    if ( poptrie->nodes ) {
        vfree(poptrie->nodes);
    }
    if ( poptrie->leaves ) {
        vfree(poptrie->leaves);
    }
    if ( poptrie->cnodes ) {
        //buddy_release(poptrie->cnodes);
        vfree(poptrie->cnodes);
    }
    if ( poptrie->cleaves ) {
      //  buddy_release(poptrie->cleaves);
        vfree(poptrie->cleaves);
    }
    if ( poptrie->dir ) {
        vfree(poptrie->dir);
    }
    if ( poptrie->altdir ) {
        vfree(poptrie->altdir);
    }
    if ( poptrie->fib.entries ) {
        vfree(poptrie->fib.entries);
    }
    if ( poptrie->_allocated ) {
        vfree(poptrie);
    }
}

/*
static inline struct fib_xid *tfxid_fxid(struct tree_fib_xid *tfxid)
{
	return likely(tfxid)
		? container_of((void *)tfxid, struct fib_xid, fx_data)
		: NULL;
}

static inline struct tree_fib_xid *fxid_tfxid(struct fib_xid *fxid)
{
	return (struct tree_fib_xid *)fxid->fx_data;
}

static inline struct fib_xid_table *txtbl_xtbl(struct tree_fib_xid_table *txtbl)
{
	return likely(txtbl)
		? container_of((void *)txtbl, struct fib_xid_table, fxt_data)
		: NULL;
}
*/
static inline struct popt_fib_xid_table *xtbl_txtbl(struct fib_xid_table *xtbl)
{
	return (struct popt_fib_xid_table *)xtbl->fxt_data;
}

static void popt_xtbl_death_work(struct work_struct *work);

#define	XIA_POPT_SZ1	19
#define	XIA_POPT_SZ0	22

static int popt_xtbl_init(struct xip_ppal_ctx *ctx, struct net *net,
			  struct xia_lock_table *locktbl,
			  const xia_ppal_all_rt_eops_t all_eops,
			  const struct xia_ppal_rt_iops *all_iops)
{
	struct fib_xid_table *new_xtbl;
	struct popt_fib_xid_table *txtbl;
	int ret;
	int i;

	printk(KERN_ALERT "in popt_xtbl_init()\n");
	if (ctx->xpc_xtbl)
		return -EEXIST; /* Duplicate. */

	new_xtbl = kzalloc(sizeof(*new_xtbl) + sizeof(*txtbl), GFP_KERNEL);
	if (!new_xtbl)
		return -ENOMEM;
	txtbl = xtbl_txtbl(new_xtbl);


	if ( NULL == txtbl ) {
	/* Allocate new one */
	txtbl =vmalloc(sizeof(struct popt_fib_xid_table));
	if ( NULL == txtbl ) {
	    return NULL;
	}
	(void)memset(txtbl, 0, sizeof(struct popt_fib_xid_table));
	/* Set the flag indicating that this data structure needs vfree() when
	   released. */
	txtbl->_allocated = 1;
	} else {
	/* Write zero's */
	(void)memset(txtbl, 0, sizeof(struct popt_fib_xid_table));
	}

	/* Allocate the nodes and leaves */
	txtbl->nodes =vmalloc(sizeof(poptrie_node_t) * (1 << XIA_POPT_SZ1));
	if ( NULL == txtbl->nodes ) {
	poptrie160_release(txtbl);
	return NULL;
	}
	txtbl->nodesz = XIA_POPT_SZ1;
	txtbl->leaves =vmalloc(sizeof(poptrie_leaf_t) * (1 << XIA_POPT_SZ0));
	if ( NULL == txtbl->leaves ) {
	poptrie160_release(txtbl);
	return NULL;
	}
	txtbl->leafsz = XIA_POPT_SZ0;

	/* Prepare the memory management system for the internal node array */
	txtbl->cnodes =vmalloc(sizeof(*txtbl->cnodes) * (1 << XIA_POPT_SZ1));
	if ( NULL == txtbl->cnodes ) {
	poptrie160_release(txtbl);
	return NULL;
	}
	txtbl->last_base1 = 0;

	/* Prepare the memory management system for the leaf node array */
	txtbl->cleaves =vmalloc(sizeof(*txtbl->cleaves) * (1 << XIA_POPT_SZ0));
	if ( NULL == txtbl->cleaves ) {
	poptrie160_release(txtbl);
	return NULL;
	}
	txtbl->last_base0 = 0;

	/* Prepare the direct pointing array */
	txtbl->dir =vmalloc(sizeof(u32) << POPTRIE_S);
	if ( NULL == txtbl->dir ) {
	poptrie160_release(txtbl);
	return NULL;
	}
	for ( i = 0; i < (1 << POPTRIE_S); i++ ) {
	txtbl->dir[i] = (u32)1 << 31;
	}

	/* Prepare the alternative direct pointing array for the update procedure */
	txtbl->altdir =vmalloc(sizeof(u32) << POPTRIE_S);
	if ( NULL == txtbl->altdir ) {
	poptrie160_release(txtbl);
	return NULL;
	}

	/* Prepare the FIB mapping table */
	txtbl->fib.entries =vmalloc(sizeof(void *) * POPTRIE_INIT_FIB_SIZE);
	if ( NULL == txtbl->fib.entries ) {
	poptrie160_release(txtbl);
	return NULL;
	}
	txtbl->fib.sz = POPTRIE_INIT_FIB_SIZE;
	txtbl->fib.n = 0;
	/* Insert a NULL entry */
	txtbl->fib.entries[txtbl->fib.n++] = NULL;

	
	new_xtbl->fxt_ppal_type = ctx->xpc_ppal_type;
	new_xtbl->fxt_net = net;
	rwlock_init(&txtbl->writers_lock);
	new_xtbl->all_eops = all_eops;
	new_xtbl->all_iops = all_iops;

	atomic_set(&new_xtbl->refcnt, 1);
	INIT_WORK(&new_xtbl->fxt_death_work, popt_xtbl_death_work);
	ctx->xpc_xtbl = new_xtbl;

	return 0;
}

static void *popt_fxid_ppal_alloc(size_t ppal_entry_size, gfp_t flags)
{
	return kmalloc(ppal_entry_size + sizeof(poptrie_node_t), flags);
}

static void popt_fxid_init(struct fib_xid *fxid, int table_id, int entry_type)
{
	printk(KERN_ALERT "Inside fxid_init");
}



static void popt_xtbl_death_work(struct work_struct *work)
{
	printk(KERN_ALERT "Inside deathWork");
	/*struct fib_xid_table *xtbl = container_of(work, struct fib_xid_table,
		fxt_death_work);
	struct tree_fib_xid_table *txtbl = xtbl_txtbl(xtbl);

	int c = atomic_read(&xtbl->fxt_count);
	int rm_count = destroy_subtree(xtbl, txtbl->root);

	* It doesn't return an error here because there's nothing
	 * the caller can do about this error/bug.
	
	if (c != rm_count) {
		pr_err("While freeing XID table of principal %x, %i entries were found, whereas %i are counted! Ignoring it, but it's a serious bug!\n",
		       __be32_to_cpu(xtbl->fxt_ppal_type), rm_count, c);
		       dump_stack();
	}

	kfree(xtbl);*/
}

/* No extra information is needed, so @parg is empty. */
static void popt_fib_unlock(struct fib_xid_table *xtbl, void *parg)
{
	printk(KERN_ALERT "Inside unlock");
}

static struct fib_xid *popt_fxid_find_rcu(struct fib_xid_table *xtbl,
					  const u8 *xid)
{
	return NULL;
}

/* No extra information is needed, so @parg is empty. */
static struct fib_xid *popt_fxid_find_lock(void *parg,
	struct fib_xid_table *xtbl, const u8 *xid)
{
	/* Do longest prefix matching matching. */
	return NULL;
}


static int popt_iterate_xids(struct fib_xid_table *xtbl,
			     int (*locked_callback)(struct fib_xid_table *xtbl,
						    struct fib_xid *fxid,
						    const void *arg),
			     const void *arg)
{
	return 0;
}


static int popt_fxid_add(struct fib_xid_table *xtbl, struct fib_xid *fxid)
{
	return 0;
}

/* No extra information is needed, so @parg is empty. */
static int popt_fxid_add_locked(void *parg, struct fib_xid_table *xtbl,
				struct fib_xid *fxid)
{
	return 0;
}
/* No extra information is needed, so @parg is empty. */
static void popt_fxid_rm_locked(void *parg, struct fib_xid_table *xtbl,
				struct fib_xid *fxid)
{
	printk(KERN_ALERT "Inside rm");
}

static void popt_fxid_rm(struct fib_xid_table *xtbl, struct fib_xid *fxid)
{
	printk(KERN_ALERT "Inside rm");
}

/* tree_xid_rm() removes the entry with the longest matching prefix,
 * since we have no prefix information for @xid.
 */
static struct fib_xid *popt_xid_rm(struct fib_xid_table *xtbl, const u8 *xid)
{
	return NULL;
}

static void popt_fxid_replace_locked(struct fib_xid_table *xtbl,
				     struct fib_xid *old_fxid,
				     struct fib_xid *new_fxid)
{
	printk(KERN_ALERT "Inside replace");
}

int popt_fib_newroute_lock(struct fib_xid *new_fxid,
			   struct fib_xid_table *xtbl,
			   struct xia_fib_config *cfg, int *padded)
{
	return 0;
}

/* tree_fib_newroute() differs from all_fib_newroute() because its lookup
 * function has the option of doing longest prefix or exact matching, and
 * all_fib_newroute() is not flexible enough to do that.
 *
 * This simple version of newroute simply adds the entry, without
 * flushing any anchors.
 */
static int popt_fib_newroute(struct fib_xid *new_fxid,
			     struct fib_xid_table *xtbl,
			     struct xia_fib_config *cfg, int *padded)
{
	return 0;
}

/* tree_fib_delroute() differs from all_fib_delroute() because its lookup
 * function has the option of doing longest prefix or exact matching, and
 * all_fib_delroute() is not flexible enough to do that.
 */
int popt_fib_delroute(struct xip_ppal_ctx *ctx, struct fib_xid_table *xtbl,
		      struct xia_fib_config *cfg)
{
	return 0;
}

/* Dump all entries in tree. */
static int popt_xtbl_dump_rcu(struct fib_xid_table *xtbl,
			      struct xip_ppal_ctx *ctx, struct sk_buff *skb,
			      struct netlink_callback *cb)
{
	return 0;
}

struct fib_xid *popt_fib_get_pred_locked(struct fib_xid *fxid)
{
	return NULL;
}

/* Main entries for LPM need to display the prefix length when dumped,
 * so tree_fib_mrd_dump() differs from fib_mrd_dump().
 */
int popt_fib_mrd_dump(struct fib_xid *fxid, struct fib_xid_table *xtbl,
		      struct xip_ppal_ctx *ctx, struct sk_buff *skb,
		      struct netlink_callback *cb)
{
	return 0;	
}

const struct xia_ppal_rt_iops xia_ppal_popt_rt_iops = {
	.xtbl_init = popt_xtbl_init,
	.xtbl_death_work = popt_xtbl_death_work,

	.fxid_ppal_alloc = popt_fxid_ppal_alloc,
	.fxid_init = popt_fxid_init,

	/* Note that there is no RCU-specific version. */
	.fxid_find_rcu = popt_fxid_find_rcu,
	.fxid_find_lock = popt_fxid_find_lock,
	.iterate_xids = popt_iterate_xids,
	/* Note that there is no RCU-specific version. */
	.iterate_xids_rcu = popt_iterate_xids,

	.fxid_add = popt_fxid_add,
	.fxid_add_locked = popt_fxid_add_locked,

	.fxid_rm = popt_fxid_rm,
	.fxid_rm_locked = popt_fxid_rm_locked,
	.xid_rm = popt_xid_rm,

	.fxid_replace_locked = popt_fxid_replace_locked,

	.fib_unlock = popt_fib_unlock,

	.fib_newroute = popt_fib_newroute,
	.fib_delroute = popt_fib_delroute,

	.xtbl_dump_rcu = popt_xtbl_dump_rcu,
};


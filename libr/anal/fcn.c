/* radare - LGPL - Copyright 2010-2017 - nibble, alvaro, pancake */

#include <r_anal.h>
#include <r_util.h>
#include <r_list.h>

#define VARPREFIX "local"
#define ARGPREFIX "arg"

#define USE_SDB_CACHE 0
#define SDB_KEY_BB "bb.0x%"PFMT64x ".0x%"PFMT64x
// XXX must be configurable by the user
#define FCN_DEPTH 512

/* speedup analysis by removing some function overlapping checks */
#define JAYRO_04 0

// 16 KB is the maximum size for a basic block
#define MAXBBSIZE 16 * 1024
#define MAX_FLG_NAME_SIZE 64

#define FIX_JMP_FWD 0
#define JMP_IS_EOB 1
#define JMP_IS_EOB_RANGE 64
#define CALL_IS_EOB 0

// 64KB max size
// 256KB max function size
#define MAX_FCN_SIZE (1024 * 256)

#define MAX_JMPTBL_SIZE 1000
#define MAX_JMPTBL_JMP 10000

#define DB a->sdb_fcns
#define EXISTS(x, ...) snprintf (key, sizeof (key) - 1, x, ## __VA_ARGS__), sdb_exists (DB, key)
#define SETKEY(x, ...) snprintf (key, sizeof (key) - 1, x, ## __VA_ARGS__);

#define VERBOSE_DELAY if (0)

#define FCN_CONTAINER(x) container_of (x, RAnalFunction, rb)
#define fcn_tree_foreach_intersect(root, it, data, from, to)										\
	for (it = _fcn_tree_iter_first (root, from, to); it.cur && (data = FCN_CONTAINER (it.cur), 1); _fcn_tree_iter_next (&(it), from, to))

typedef struct fcn_tree_iter_t {
	int len;
	RBNode *cur;
	RBNode *path[R_RBTREE_MAX_HEIGHT];
} FcnTreeIter;

#if USE_SDB_CACHE
static Sdb *HB = NULL;
#endif

R_API const char *r_anal_fcn_type_tostring(int type) {
	switch (type) {
	case R_ANAL_FCN_TYPE_NULL: return "null";
	case R_ANAL_FCN_TYPE_FCN: return "fcn";
	case R_ANAL_FCN_TYPE_LOC: return "loc";
	case R_ANAL_FCN_TYPE_SYM: return "sym";
	case R_ANAL_FCN_TYPE_IMP: return "imp";
	case R_ANAL_FCN_TYPE_INT: return "int"; // interrupt
	case R_ANAL_FCN_TYPE_ROOT: return "root";
	}
	return "unk";
}

static int cmpaddr(const void *_a, const void *_b) {
	const RAnalBlock *a = _a, *b = _b;
	return (a->addr - b->addr);
}

R_API void r_anal_fcn_update_tinyrange_bbs(RAnalFunction *fcn) {
	RAnalBlock *bb;
	RListIter *iter;
	r_list_sort (fcn->bbs, &cmpaddr);
	r_tinyrange_fini (&fcn->bbr);
	r_list_foreach (fcn->bbs, iter, bb) {
		r_tinyrange_add (&fcn->bbr, bb->addr, bb->addr + bb->size);
	}
}

// _fcn_tree_{cmp_addr,calc_max_addr,free,probe} are used by interval tree.
static int _fcn_tree_cmp_addr(const void *a_, const RBNode *b_) {
	const RAnalFunction *a = (const RAnalFunction *)a_,
		*b = container_of (b_, const RAnalFunction, rb);
	ut64 from0 = a->addr, to0 = a->addr + a->_size,
		from1 = b->addr, to1 = b->addr + b->_size;
	if (from0 != from1) {
		return from0 < from1 ? -1 : 1;
	}
	if (to0 != to1) {
		return to0 - 1 < to1 - 1 ? -1 : 1;
	}
	return 0;
}

static void _fcn_tree_calc_max_addr(RBNode *node) {
	RAnalFunction *fcn = FCN_CONTAINER (node), *fcn1;
	int i;
	fcn->rb_max_addr = fcn->addr + (fcn->_size == 0 ? 0 : fcn->_size - 1);
	for (i = 0; i < 2; i++) {
		if (node->child[i]) {
			fcn1 = container_of (node->child[i], RAnalFunction, rb);
			if (fcn1->rb_max_addr > fcn->rb_max_addr) {
				fcn->rb_max_addr = fcn1->rb_max_addr;
			}
		}
	}
}

static void _fcn_tree_free(RBNode *node) {
	// TODO RB tree is an intrusive data structure by embedding RBNode into RAnalFunction.
	// Currently fcns takes the ownership of the resources.
	// If the ownership transfers from fcns to fcn_tree:
	//
	// r_anal_fcn_free (FCN_CONTAINER (node));
}

// Descent x_ to find the first node whose interval intersects [from, to)
static RBNode *_fcn_tree_probe(FcnTreeIter *it, RBNode *x_, ut64 from, ut64 to) {
	RAnalFunction *x = FCN_CONTAINER (x_), *y;
	RBNode *y_;
	for (;;) {
		if ((y_ = x_->child[0]) && (y = FCN_CONTAINER (y_), from <= y->rb_max_addr)) {
			it->path[it->len++] = x_;
			x_ = y_;
			x = y;
			continue;
		}
		if (x->addr <= to - 1) {
			if (from <= x->addr + (x->_size == 0 ? 0 : x->_size - 1)) {
				return x_;
			}
			if ((y_ = x_->child[1])) {
				x_ = y_;
				x = FCN_CONTAINER (y_);
				if (from <= x->rb_max_addr) {
					continue;
				}
			}
		}
		return NULL;
	}
}

R_API bool r_anal_fcn_tree_delete(RBNode **root, RAnalFunction *data) {
	return r_rbtree_aug_delete (root, data, _fcn_tree_cmp_addr, _fcn_tree_free, _fcn_tree_calc_max_addr);
}

R_API void r_anal_fcn_tree_insert(RBNode **root, RAnalFunction *fcn) {
	r_rbtree_aug_insert (root, fcn, &(fcn->rb), _fcn_tree_cmp_addr, _fcn_tree_calc_max_addr);
}

// Find RAnalFunction whose addr is equal to addr
static RAnalFunction *_fcn_tree_find_addr(RBNode *x_, ut64 addr) {
	while (x_) {
		RAnalFunction *x = FCN_CONTAINER (x_);
		if (x->addr == addr) {
			return x;
		}
		x_ = x_->child[x->addr < addr];
	}
	return NULL;
}

// _fcn_tree_{iter_first,iter_next} are used to iterate functions whose intervals intersect [from, to) in O(log(n) + |candidates|) time
static FcnTreeIter _fcn_tree_iter_first(RBNode *x_, ut64 from, ut64 to) {
	FcnTreeIter it = {0};
	it.len = 0;
	if (x_ && from <= FCN_CONTAINER (x_)->rb_max_addr) {
		it.cur = _fcn_tree_probe (&it, x_, from, to);
	} else {
		it.cur = NULL;
	}
	return it;
}

static void _fcn_tree_iter_next(FcnTreeIter *it, ut64 from, ut64 to) {
	RBNode *x_ = it->cur, *y_;
	RAnalFunction *x = FCN_CONTAINER (x_), *y;
	for (;;) {
		if ((y_ = x_->child[1]) && (y = FCN_CONTAINER (y_), from <= y->rb_max_addr)) {
			it->cur = _fcn_tree_probe (it, y_, from, to);
			break;
		}
		if (!it->len) {
			it->cur = NULL;
			break;
		}
		x_ = it->path[--it->len];
		x = FCN_CONTAINER (x_);
		if (to - 1 < x->addr) {
			it->cur = NULL;
			break;
		}
		if (from <= x->addr + (x->_size == 0 ? 0 : x->_size - 1)) {
			it->cur = x_;
			break;
		}
	}
}

R_API int r_anal_fcn_resize(RAnalFunction *fcn, int newsize) {
	ut64 eof; /* end of function */
	RAnalBlock *bb;
	RListIter *iter, *iter2;
	if (!fcn || newsize < 1) {
		return false;
	}
	r_anal_fcn_set_size (fcn, newsize);
	eof = fcn->addr + r_anal_fcn_size (fcn);
	r_list_foreach_safe (fcn->bbs, iter, iter2, bb) {
		if (bb->addr >= eof) {
			// already called by r_list_delete r_anal_bb_free (bb);
			r_list_delete (fcn->bbs, iter);
			continue;
		}
		if (bb->addr + bb->size >= eof) {
			bb->size = eof - bb->addr;
		}
		if (bb->jump != UT64_MAX && bb->jump >= eof) {
			bb->jump = UT64_MAX;
		}
		if (bb->fail != UT64_MAX && bb->fail >= eof) {
			bb->fail = UT64_MAX;
		}
	}
	r_anal_fcn_update_tinyrange_bbs (fcn);
	return true;
}

R_API RAnalFunction *r_anal_fcn_new() {
	RAnalFunction *fcn = R_NEW0 (RAnalFunction);
	if (!fcn) {
		return NULL;
	}
	/* Function return type */
	fcn->rets = 0;
	fcn->_size = 0;
	/* Function qualifier: static/volatile/inline/naked/virtual */
	fcn->fmod = R_ANAL_FQUALIFIER_NONE;
	/* Function calling convention: cdecl/stdcall/fastcall/etc */
	fcn->cc = NULL;
	/* Function attributes: weak/noreturn/format/etc */
	fcn->addr = UT64_MAX;
#if FCN_OLD
	fcn->refs = r_anal_ref_list_new ();
	fcn->xrefs = r_anal_ref_list_new ();
#endif
	fcn->fcn_locs = NULL;
	fcn->bbs = r_anal_bb_list_new ();
	fcn->fingerprint = NULL;
	fcn->diff = r_anal_diff_new ();
	r_tinyrange_init (&fcn->bbr);
	return fcn;
}

R_API RList *r_anal_fcn_list_new() {
	RList *list = r_list_new ();
	if (!list) {
		return NULL;
	}
	list->free = &r_anal_fcn_free;
	return list;
}

R_API void r_anal_fcn_free(void *_fcn) {
	RAnalFunction *fcn = _fcn;
	if (!_fcn) {
		return;
	}
	fcn->_size = 0;
	free (fcn->name);
	free (fcn->attr);
	r_tinyrange_fini (&fcn->bbr);
#if FCN_OLD
	r_list_free (fcn->refs);
	r_list_free (fcn->xrefs);
#endif
	// all functions are freed in anal->fcns
	fcn->fcn_locs = NULL;
	if (fcn->bbs) {
		fcn->bbs->free = (RListFree) r_anal_bb_free;
		r_list_free (fcn->bbs);
		fcn->bbs = NULL;
	}
	free (fcn->fingerprint);
	r_anal_diff_free (fcn->diff);
	free (fcn->args);
	free (fcn);
}

static bool refExists(RList *refs, RAnalRef *ref) {
	RAnalRef *r;
	RListIter *iter;
	r_list_foreach (refs, iter, r) {
		if (r && r->at == ref->at && ref->addr == r->addr) {
			r->type = ref->type;
			return true;
		}
	}
	return false;
}

R_API int r_anal_fcn_xref_add(RAnal *a, RAnalFunction *fcn, ut64 at, ut64 addr, int type) {
	RAnalRef *ref;
	if (!fcn || !a) {
		return false;
	}
	if (!a->iob.is_valid_offset (a->iob.io, addr, 0)) {
		return false;
	}
	ref = r_anal_ref_new ();
	if (!ref) {
		return false;
	}
	// set global reference
// r_cons_printf ("C 0x%llx  0x%llx\n", at, addr);
	r_anal_xrefs_set (a, type, at, addr);
	// set per-function reference
#if FCN_OLD
// TOO OLD we shouldnt be storing this.. or we do?
	ref->at = at; // from
	ref->addr = addr; // to
	ref->type = type;
	// TODO: ensure we are not dupping xrefs
	if (refExists (fcn->refs, ref)) {
		r_anal_ref_free (ref);
	} else {
		r_list_append (fcn->refs, ref);
	}
#endif
#if FCN_SDB
	sdb_add (DB, sdb_fmt (0, "fcn.0x%08"PFMT64x ".name", fcn->addr), fcn->name, 0);
	// encode the name in base64 ?
	sdb_num_add (DB, sdb_fmt (0, "fcn.name.%s", fcn->name), fcn->addr, 0);
	sdb_array_add_num (DB, sdb_fmt (0, "fcn.0x%08"PFMT64x ".xrefs", fcn->addr), at, 0);
#endif
	return true;
}

R_API int r_anal_fcn_xref_del(RAnal *a, RAnalFunction *fcn, ut64 at, ut64 addr, int type) {
#if FCN_OLD
	RAnalRef *ref;
	RListIter *iter;
	/* No need for _safe loop coz we return immediately after the delete. */
	r_list_foreach (fcn->xrefs, iter, ref) {
		if ((type != -1 || type == ref->type) &&
		(at == 0LL || at == ref->at) &&
		(addr == 0LL || addr == ref->addr)) {
			r_list_delete (fcn->xrefs, iter);
			return true;
		}
	}
#endif
#if FCN_SDB
	// TODO
	// sdb_array_delete_num (DB, key, at, 0);
#endif
	return false;
}

static RAnalBlock *bbget(RAnalFunction *fcn, ut64 addr) {
	RListIter *iter;
	RAnalBlock *bb;
	r_list_foreach (fcn->bbs, iter, bb) {
		ut64 eaddr = bb->addr + bb->size;
		if (bb->addr >= eaddr && addr == bb->addr) {
			return bb;
		}
		if ((addr >= bb->addr) && (addr < eaddr)) {
			return bb;
		}
	}
	return NULL;
}

static RAnalBlock *appendBasicBlock(RAnal *anal, RAnalFunction *fcn, ut64 addr) {
	RAnalBlock *bb = r_anal_bb_new ();
	if (!bb) {
		return NULL;
	}
	bb->addr = addr;
	bb->size = 0;
	bb->jump = UT64_MAX;
	bb->fail = UT64_MAX;
	bb->type = 0; // TODO
	r_anal_fcn_bbadd (fcn, bb);
	if (anal->cb.on_fcn_bb_new) {
		anal->cb.on_fcn_bb_new (anal, anal->user, fcn, bb);
	}
	return bb;
}

#define FITFCNSZ() {\
		st64 n = bb->addr + bb->size - fcn->addr;\
		if (n >= 0 && r_anal_fcn_size (fcn) < n) { r_anal_fcn_set_size (fcn, n); } }\
	if (r_anal_fcn_size (fcn) > MAX_FCN_SIZE) {\
		/* eprintf ("Function too big at 0x%"PFMT64x" + %d\n", bb->addr, fcn->size); */\
		r_anal_fcn_set_size (fcn, 0);\
		return R_ANAL_RET_ERROR; }

// ETOOSLOW
static char *get_varname(RAnal *a, RAnalFunction *fcn, char type, const char *pfx, int idx) {
	char *varname = r_str_newf ("%s_%xh", pfx, idx);
	int i = 2;
	while (1) {
		RAnalVar *v = r_anal_var_get_byname (a, fcn, varname);
		if (!v) {
			v = r_anal_var_get_byname (a, fcn, varname);
		}
		if (!v) {
			v = r_anal_var_get_byname (a, fcn, varname);
		}
		if (!v) {
			break;
		}
		if (v->kind == type && R_ABS (v->delta) == idx) {
			r_anal_var_free (v);
			break;
		}
		r_anal_var_free (v);
		free (varname);
		varname = r_str_newf ("%s_%xh_%d", pfx, idx, i);
		i++;
	}
	return varname;
}

static int fcn_recurse(RAnal *anal, RAnalFunction *fcn, ut64 addr, ut8 *buf, ut64 len, int depth);
#define recurseAt(x) {\
		ut8 *bbuf = malloc (MAXBBSIZE);\
		anal->iob.read_at (anal->iob.io, x, bbuf, MAXBBSIZE);\
		ret = fcn_recurse (anal, fcn, x, bbuf, MAXBBSIZE, depth - 1);\
		r_anal_fcn_update_tinyrange_bbs (fcn);\
		free (bbuf);\
}

static int try_walkthrough_jmptbl(RAnal *anal, RAnalFunction *fcn, int depth, ut64 ip, ut64 ptr, int ret0) {
	int ret = ret0;
	ut8 *jmptbl = malloc (MAX_JMPTBL_SIZE);
	ut64 jmpptr, offs, sz = anal->bits >> 3;
	if (!jmptbl) {
		return 0;
	}
	anal->iob.read_at (anal->iob.io, ptr, jmptbl, MAX_JMPTBL_SIZE);
	for (offs = 0; offs + sz - 1 < MAX_JMPTBL_SIZE; offs += sz) {
		switch (sz) {
		case 1:
			jmpptr = r_read_le8 (jmptbl + offs);
			break;
		case 2:
			jmpptr = r_read_le16 (jmptbl + offs);
			break;
		case 4:
			jmpptr = r_read_le32 (jmptbl + offs);
			break;
		case 8:
			jmpptr = r_read_le32 (jmptbl + offs);
			break; // XXX
		default:
			jmpptr = r_read_le64 (jmptbl + offs);
			break;
		}
		if (!anal->iob.is_valid_offset (anal->iob.io, jmpptr, 0)) {
			jmpptr = ptr + (st32) jmpptr;
			if (!anal->iob.is_valid_offset (anal->iob.io, jmpptr, 0)) {
				break;
			}
		}
		if (anal->limit) {
			if (jmpptr < anal->limit->from || jmpptr > anal->limit->to) {
				break;
			}
		}
		// if (jmpptr < ip - MAX_JMPTBL_JMP || jmpptr > ip + MAX_JMPTBL_JMP) { break; }
		recurseAt (jmpptr);
	}
	free (jmptbl);
	return ret;
}

static ut64 search_reg_val(RAnal *anal, ut8 *buf, ut64 len, ut64 addr, char *regsz) {
	ut64 offs, oplen;
	RAnalOp op = {
		0
	};
	ut64 ret = UT64_MAX;
	const int addrbytes = anal->iob.io ? anal->iob.io->addrbytes : 1;
	for (offs = 0; offs < len; offs += addrbytes * oplen) {
		r_anal_op_fini (&op);
		if ((oplen = r_anal_op (anal, &op, addr + offs, buf + offs, len - offs)) < 1) {
			break;
		}
		if (op.dst && op.dst->reg && op.dst->reg->name && !strcmp (op.dst->reg->name, regsz)) {
			if (op.src[0]) {
				ret = op.src[0]->delta;
			}
		}
	}
	return ret;
}

#define gotoBeach(x) ret = x; goto beach;
#define gotoBeachRet() goto beach;

static void extract_arg(RAnal *anal, RAnalFunction *fcn, RAnalOp *op, const char *reg, const char *sign, char type) {
	char sigstr[16] = {0};
	st64 ptr;
	char *addr;
	if (!anal || !fcn || !op) {
		return;
	}
	// snprintf (sigstr, sizeof (sigstr), ",%s,%s", reg, sign);
	snprintf (sigstr, sizeof (sigstr), ",%s,%s", reg, sign);
	const char *op_esil = r_strbuf_get (&op->esil);
	if (!op_esil) {
		return;
	}
	char *esil_buf = strdup (op_esil);
	if (!esil_buf) {
		return;
	}
	char *ptr_end = strstr (esil_buf, sigstr);
	if (!ptr_end) {
		free (esil_buf);
		return;
	}
#if 1
	*ptr_end = 0;
	addr = ptr_end;
	while ((addr[0] != '0' || addr[1] != 'x') && addr >= esil_buf + 1 && *addr != ',') {
		addr--;
	}
	if (strncmp (addr, "0x", 2)) {
		free (esil_buf);
		return;
	}
	ptr = (st64) r_num_get (NULL, addr);
#else
	ptr = -op->ptr;
	if (ptr%4) {
		free (esil_buf);
		return;
	}
#endif
	if (*sign == '+') {
		const char *pfx = (ptr < fcn->maxstack && type == 's')? VARPREFIX: ARGPREFIX;
		char *varname = get_varname (anal, fcn, type, pfx, R_ABS (ptr));
		r_anal_var_add (anal, fcn->addr, 1, ptr, type, NULL, anal->bits / 8, varname);
		r_anal_var_access (anal, fcn->addr, type, 1, ptr, 0, op->addr);
		free (varname);
	} else {
		char *varname = get_varname (anal, fcn, type, VARPREFIX, R_ABS (ptr));
		r_anal_var_add (anal, fcn->addr, 1, -ptr, type, NULL, anal->bits / 8, varname);
		r_anal_var_access (anal, fcn->addr, type, 1, -ptr, 1, op->addr);
		free (varname);
	}
	free (esil_buf);
}

R_API void r_anal_fcn_fill_args(RAnal *anal, RAnalFunction *fcn, RAnalOp *op) {
	if (!anal || !fcn || !op) {
		return;
	}
	const char *BP = anal->reg->name[R_REG_NAME_BP];
	const char *SP =  anal->reg->name[R_REG_NAME_SP];
	extract_arg (anal, fcn, op, BP, "+", 'b');
	extract_arg (anal, fcn, op, BP, "-", 'b');
	extract_arg (anal, fcn, op, SP, "+", 's');
}

static bool isInvalidMemory(const ut8 *buf, int len) {
	// can be wrong
	return !memcmp (buf, "\xff\xff\xff\xff", R_MIN (len, 4));
	// return buf[0]==buf[1] && buf[0]==0xff && buf[2]==0xff && buf[3] == 0xff;
}

static bool is_delta_pointer_table(RAnal *anal, ut64 addr, ut64 ptr) {
	int i;
	ut64 dst;
	st32 jmptbl[64] = {
		0
	};
	/* check if current instruction is followed by an ujmp */
	ut8 buf[64];
	anal->iob.read_at (anal->iob.io, addr, (ut8 *) buf, 64);
	RAnalOp aop = {0};
	bool isValid = false;
	for (i = 0; i + 8 < 64; i++) {
		int len = r_anal_op (anal, &aop, addr + i, buf + i, 64 - i);
		if (len < 1) {
			len = 1;
		}
		if (aop.type == R_ANAL_OP_TYPE_UJMP || aop.type == R_ANAL_OP_TYPE_RJMP) {
			isValid = true;
			break;
		}
		i += len - 1;
	}
	if (!isValid) {
		return false;
	}
	
	/* check if jump table contains valid deltas */
	anal->iob.read_at (anal->iob.io, ptr, (ut8 *) &jmptbl, 64);
	// XXX this is not endian safe
	for (i = 0; i < 4; i++) {
		dst = ptr + jmptbl[0];
		if (!anal->iob.is_valid_offset (anal->iob.io, dst, 0)) {
			return false;
		}
	}
	return true;
}

static bool regs_exist(RAnalValue *src, RAnalValue *dst) {
	return src && dst && src->reg && dst->reg && src->reg->name && dst->reg->name;
}

// 0 if not skipped; 1 if skipped; 2 if skipped before
static int skip_hp(RAnal *anal, RAnalFunction *fcn, RAnalOp *op, RAnalBlock *bb, ut64 addr,
                   char *tmp_buf, int oplen, int un_idx, int *idx) {
	// this step is required in order to prevent infinite recursion in some cases
	if ((addr + un_idx - oplen) == fcn->addr) {
		if (!anal->flb.exist_at (anal->flb.f, "skip", 4, op->addr)) {
			snprintf (tmp_buf + 5, MAX_FLG_NAME_SIZE - 6, "%"PFMT64u, op->addr);
			anal->flb.set (anal->flb.f, tmp_buf, op->addr, oplen);
			fcn->addr += oplen;
			bb->size -= oplen;
			bb->addr += oplen;
			*idx = un_idx;
			return 1;
		}
		return 2;
	}
	return 0;
}

R_API int r_anal_case(RAnal *anal, RAnalFunction *fcn, ut64 addr_bbsw, ut64 addr, ut8 *buf, ut64 len, int reftype) {
	RAnalOp op = {
		0
	};
	int oplen, idx = 0;
	while (idx < len) {
		if ((len - idx) < 5) {
			break;
		}
		r_anal_op_fini (&op);
		if ((oplen = r_anal_op (anal, &op, addr + idx, buf + idx, len - idx)) < 1) {
			return 0;
		}
		switch (op.type) {
		case R_ANAL_OP_TYPE_TRAP:
		case R_ANAL_OP_TYPE_RET:
		case R_ANAL_OP_TYPE_JMP:
			// eprintf ("CASE AT 0x%llx size %d\n", addr, idx + oplen);
			anal->cmdtail = r_str_appendf (anal->cmdtail, "afb+ 0x%"PFMT64x " 0x%"PFMT64x " %d\n",
				fcn->addr, addr, idx + oplen);
			anal->cmdtail = r_str_appendf (anal->cmdtail, "afbe 0x%"PFMT64x " 0x%"PFMT64x "\n",
				addr_bbsw, addr);
			return idx + oplen;
		}
		idx += oplen;
	}
	return idx;
}

static int walk_switch(RAnal *anal, RAnalFunction *fcn, ut64 from, ut64 at) {
	ut8 buf[1024];
	int i;
	eprintf ("WALK SWITCH TABLE INTO (0x%"PFMT64x ") %"PFMT64x "\n", from, at);
	for (i = 0; i < 10; i++) {
		anal->iob.read_at (anal->iob.io, at, buf, sizeof (buf));
		int sz = r_anal_case (anal, fcn, from, at, buf, sizeof (buf), 0);
		if (sz < 1) {
			break;
		}
		at += sz;
	}
	return 0;
}

static int fcn_recurse(RAnal *anal, RAnalFunction *fcn, ut64 addr, ut8 *buf, ut64 len, int depth) {
	const int continue_after_jump = anal->opt.afterjmp;
	const int noncode = anal->opt.noncode;
	const int addrbytes = anal->iob.io ? anal->iob.io->addrbytes : 1;
	RAnalBlock *bb = NULL;
	RAnalBlock *bbg = NULL;
	int ret = R_ANAL_RET_END, skip_ret = 0;
	int overlapped = 0;
	char *varname;
	RAnalOp op = {
		0
	};
	int oplen, idx = 0;
	struct {
		int cnt;
		int idx;
		int after;
		int pending;
		int adjust;
		int un_idx; // delay.un_idx
	} delay = {
		0
	};
	char tmp_buf[MAX_FLG_NAME_SIZE + 5] = "skip";

	if (anal->sleep) {
		r_sys_usleep (anal->sleep);
	}

	if (depth < 1) {
		return R_ANAL_RET_ERROR; // MUST BE TOO DEEP
	}

	if (!noncode && anal->cur && anal->cur->is_valid_offset && !anal->cur->is_valid_offset (anal, addr, 0)) {
		return R_ANAL_RET_END;
	}

	// check if address is readable //:
	if (!anal->iob.is_valid_offset (anal->iob.io, addr, 0)) {
		if (addr != UT64_MAX && !anal->iob.io->va) {
			eprintf ("Invalid address 0x%"PFMT64x ". Try with io.va=true\n", addr);
		}
		return R_ANAL_RET_ERROR; // MUST BE TOO DEEP
	}

	if (r_anal_get_fcn_at (anal, addr, 0)) {
		return R_ANAL_RET_ERROR; // MUST BE NOT FOUND
	}
	bb = bbget (fcn, addr);
	if (bb) {
		r_anal_fcn_split_bb (anal, fcn, bb, addr);
		if (anal->opt.recont) {
			return R_ANAL_RET_END;
		}
		return R_ANAL_RET_ERROR; // MUST BE NOT DUP
	}

	bb = appendBasicBlock (anal, fcn, addr);

	VERBOSE_ANAL eprintf ("Append bb at 0x%08"PFMT64x" (fcn 0x%08"PFMT64x ")\n", addr, fcn->addr);

	bool last_is_push = false;
	ut64 last_push_addr = UT64_MAX;
	if (anal->limit && addr + idx < anal->limit->from) {
		return R_ANAL_RET_END;
	}
	while (addrbytes * idx < len) {
		if (anal->limit && anal->limit->to <= addr + idx) {
			break;
		}
repeat:
		if ((len - addrbytes * idx) < 5) {
			break;
		}
		r_anal_op_fini (&op);
		if (isInvalidMemory (buf + addrbytes * idx, len - addrbytes * idx)) {
			FITFCNSZ ();
			VERBOSE_ANAL eprintf ("FFFF opcode at 0x%08"PFMT64x "\n", addr + idx);
			return R_ANAL_RET_ERROR;
		}
		// check if opcode is in another basic block
		// in that case we break
		if ((oplen = r_anal_op (anal, &op, addr + idx, buf + addrbytes * idx, len - addrbytes * idx)) < 1) {
			VERBOSE_ANAL eprintf ("Unknown opcode at 0x%08"PFMT64x "\n", addr + idx);
			if (!idx) {
				gotoBeach (R_ANAL_RET_END);
			} else {
				break; // unspecified behaviour
			}
		}
		if (idx > 0 && !overlapped) {
			bbg = bbget (fcn, addr + idx);
			if (bbg && bbg != bb) {
				bb->jump = addr + idx;
				overlapped = 1;
				VERBOSE_ANAL eprintf("Overlapped at 0x%08"PFMT64x "\n", addr + idx);
				// return R_ANAL_RET_END;
			}
		}
		if (!overlapped) {
			r_anal_bb_set_offset (bb, bb->ninstr++, addr + idx - bb->addr);
			bb->size += oplen;
			fcn->ninstr++;
			// FITFCNSZ(); // defer this, in case this instruction is a branch delay entry
			// fcn->size += oplen; /// XXX. must be the sum of all the bblocks
		}
		idx += oplen;
		delay.un_idx = idx;
		if (op.delay > 0 && !delay.pending) {
			// Handle first pass through a branch delay jump:
			// Come back and handle the current instruction later.
			// Save the location of it in `delay.idx`
			// note, we have still increased size of basic block
			// (and function)
			VERBOSE_DELAY eprintf("Enter branch delay at 0x%08"PFMT64x ". bb->sz=%d\n", addr + idx - oplen, bb->size);
			delay.idx = idx - oplen;
			delay.cnt = op.delay;
			delay.pending = 1; // we need this in case the actual idx is zero...
			delay.adjust = !overlapped; // adjustment is required later to avoid double count
			continue;
		}

		if (delay.cnt > 0) {
			// if we had passed a branch delay instruction, keep
			// track of how many still to process.
			delay.cnt--;
			if (!delay.cnt) {
				VERBOSE_DELAY eprintf("Last branch delayed opcode at 0x%08"PFMT64x ". bb->sz=%d\n", addr + idx - oplen, bb->size);
				delay.after = idx;
				idx = delay.idx;
				// At this point, we are still looking at the
				// last instruction in the branch delay group.
				// Next time, we will again be looking
				// at the original instruction that entered
				// the branch delay.
			}
		} else if (op.delay > 0 && delay.pending) {
			VERBOSE_DELAY eprintf("Revisit branch delay jump at 0x%08"PFMT64x ". bb->sz=%d\n", addr + idx - oplen, bb->size);
			// This is the second pass of the branch delaying opcode
			// But we also already counted this instruction in the
			// size of the current basic block, so we need to fix that
			if (delay.adjust) {
				bb->size -= oplen;
				fcn->ninstr--;
				VERBOSE_DELAY eprintf("Correct for branch delay @ %08"PFMT64x " bb.addr=%08"PFMT64x " corrected.bb=%d f.uncorr=%d\n",
				addr + idx - oplen, bb->addr, bb->size, r_anal_fcn_size (fcn));
				FITFCNSZ ();
			}
			// Next time, we go to the opcode after the delay count
			// Take care not to use this below, use delay.un_idx instead ...
			idx = delay.after;
			delay.pending = delay.after = delay.idx = delay.adjust = 0;
		}
		// Note: if we got two branch delay instructions in a row due to an
		// compiler bug or junk or something it wont get treated as a delay
		/* TODO: Parse fastargs (R_ANAL_VAR_ARGREG) */
		switch (op.stackop) {
		case R_ANAL_STACK_INC:
			if (R_ABS (op.stackptr) < 8096) {
				fcn->stack += op.stackptr;
				if (fcn->stack > fcn->maxstack) {
					fcn->maxstack = fcn->stack;
				}
			}
			bb->stackptr += op.stackptr;
			break;
		case R_ANAL_STACK_RESET:
			bb->stackptr = 0;
			break;
		// TODO: use fcn->maxstack to know our stackframe
		case R_ANAL_STACK_SET:
			if ((int) op.ptr > 0) {
				varname = get_varname (anal, fcn, 'b', ARGPREFIX, R_ABS (op.ptr));
			} else {
				varname = get_varname (anal, fcn, 'b', VARPREFIX, R_ABS (op.ptr));
			}
			r_anal_var_add (anal, fcn->addr, 1, op.ptr, 'b', NULL, anal->bits / 8, varname);
			r_anal_var_access (anal, fcn->addr, 'b', 1, op.ptr, 1, op.addr);
			free (varname);
			break;
		// TODO: use fcn->maxstack to know our stackframe
		case R_ANAL_STACK_GET:
			if (((int) op.ptr) > 0) {
				varname = get_varname (anal, fcn, 'b', ARGPREFIX, R_ABS (op.ptr));
			} else {
				varname = get_varname (anal, fcn, 'b', VARPREFIX, R_ABS (op.ptr));
			}
			r_anal_var_add (anal, fcn->addr, 1, op.ptr, 'b', NULL, anal->bits / 8, varname);
			r_anal_var_access (anal, fcn->addr, 'b', 1, op.ptr, 0, op.addr);
			free (varname);
			break;
		}

		if (op.ptr && op.ptr != UT64_MAX && op.ptr != UT32_MAX) {
			// swapped parameters wtf
			r_anal_fcn_xref_add (anal, fcn, op.addr, op.ptr, R_ANAL_REF_TYPE_DATA);
		}

		switch (op.type & R_ANAL_OP_TYPE_MASK) {
		case R_ANAL_OP_TYPE_MOV:
			// skip mov reg,reg
			if (anal->opt.hpskip && regs_exist (op.src[0], op.dst)
			&& !strcmp (op.src[0]->reg->name, op.dst->reg->name)) {
				skip_ret = skip_hp (anal, fcn, &op, bb, addr, tmp_buf, oplen, delay.un_idx, &idx);
				if (skip_ret == 1) {
					goto repeat;
				}
				if (skip_ret == 2) {
					return R_ANAL_RET_END;
				}
			}
			break;
		case R_ANAL_OP_TYPE_LEA:
			// skip lea reg,[reg]
			if (anal->opt.hpskip && regs_exist (op.src[0], op.dst)
			&& !strcmp (op.src[0]->reg->name, op.dst->reg->name)) {
				skip_ret = skip_hp (anal, fcn, &op, bb, addr, tmp_buf, oplen, delay.un_idx, &idx);
				if (skip_ret == 1) {
					goto repeat;
				}
				if (skip_ret == 2) {
					return R_ANAL_RET_END;
				}
			}
			if (anal->opt.jmptbl) {
				if (is_delta_pointer_table (anal, op.addr, op.ptr)) {
					char *str = r_str_newf ("pxt. 0x%08" PFMT64x" @ 0x%08"PFMT64x "\n", op.addr, op.ptr);
					if (!anal->cmdtail) {
						anal->cmdtail = r_str_appendf (anal->cmdtail, str);
					}
					if (anal->cmdtail && !strstr (anal->cmdtail, str)) {
						anal->cmdtail = r_str_appendf (anal->cmdtail, str);
					} 
					free (str);
					// jmptbl_addr = op.ptr;
					// jmptbl_size = -1;
					// ret = try_walkthrough_jmptbl (anal, fcn, depth, op.addr, op.ptr, 4);
				}
			}
			break;
		case R_ANAL_OP_TYPE_ILL:
			if (anal->opt.nopskip && len > 3 && !memcmp (buf, "\x00\x00\x00\x00", 4)) {
				if ((addr + delay.un_idx - oplen) == fcn->addr) {
					fcn->addr += oplen;
					bb->size -= oplen;
					bb->addr += oplen;
					idx = delay.un_idx;
					goto repeat;
				} else {
					// sa
					bb->size -= oplen;
					op.type = R_ANAL_OP_TYPE_RET;
				}
			}
			FITFCNSZ ();
			r_anal_op_fini (&op);
			gotoBeach (R_ANAL_RET_END);
			break;
		case R_ANAL_OP_TYPE_TRAP:
			if (anal->opt.nopskip && buf[0] == 0xcc) {
				if ((addr + delay.un_idx - oplen) == fcn->addr) {
					fcn->addr += oplen;
					bb->size -= oplen;
					bb->addr += oplen;
					idx = delay.un_idx;
					goto repeat;
				}
			}
			FITFCNSZ ();
			r_anal_op_fini (&op);
			return R_ANAL_RET_END;
		case R_ANAL_OP_TYPE_NOP:
			if (anal->opt.nopskip) {
				if (!strcmp (anal->cur->arch, "mips")) {
					// Looks like this flags check is useful only for mips
					// do not skip nops if there's a flag at starting address
					RFlagItem *fi = anal->flb.get_at (anal->flb.f, addr, false);
					if (!fi || strncmp (fi->name, "sym.", 4)) {
						if ((addr + delay.un_idx - oplen) == fcn->addr) {
							fcn->addr += oplen;
							bb->size -= oplen;
							bb->addr += oplen;
							idx = delay.un_idx;
							goto repeat;
						}
					}
				} else {
					RFlagItem *fi = anal->flb.get_at (anal->flb.f, fcn->addr? fcn->addr: addr, false);
					if (fi) {
						break;
					}
					skip_ret = skip_hp (anal, fcn, &op, bb, addr, tmp_buf, oplen, delay.un_idx, &idx);
					if (skip_ret == 1) {
						goto repeat;
					}
					if (skip_ret == 2) {
						return R_ANAL_RET_END;
					}
				}
			}
			break;
		case R_ANAL_OP_TYPE_JMP:
			{
				RFlagItem *fi = anal->flb.get_at (anal->flb.f, op.jump, false);
				if (fi && strstr (fi->name, "imp.")) {
					FITFCNSZ ();
					r_anal_op_fini (&op);
					return R_ANAL_RET_END;
				}
			}
			if (op.jump == UT64_MAX) {
				FITFCNSZ ();
				r_anal_op_fini (&op);
				return R_ANAL_RET_END;
			}
			if (anal->opt.jmpref) {
				(void) r_anal_fcn_xref_add (anal, fcn, op.addr, op.jump, R_ANAL_REF_TYPE_CODE);
			}
			if (!anal->opt.jmpabove && (op.jump < fcn->addr)) {
				FITFCNSZ ();
				r_anal_op_fini (&op);
				return R_ANAL_RET_END;
			}
			if (r_anal_noreturn_at (anal, op.jump)) {
				FITFCNSZ ();
				r_anal_op_fini (&op);
				return R_ANAL_RET_END;
			}
			{
				bool must_eob = anal->opt.eobjmp;
				if (!must_eob) {
					RIOSection *s = anal->iob.sect_vget (anal->iob.io, addr);
					if (s) {
						must_eob = (op.jump < s->vaddr || op.jump >= s->vaddr + s->vsize);
					}
				}
				if (must_eob) {
					FITFCNSZ ();
					op.jump = UT64_MAX;
					recurseAt (op.jump);
					recurseAt (op.fail);
					gotoBeachRet ();
					return R_ANAL_RET_END;
				}
			}
			if (anal->opt.bbsplit) {
#if FIX_JMP_FWD
				bb->jump = op.jump;
				bb->fail = UT64_MAX;
				FITFCNSZ ();
				return R_ANAL_RET_END;
#else
				if (!overlapped) {
					bb->jump = op.jump;
					bb->fail = UT64_MAX;
				}
				recurseAt (op.jump);
				FITFCNSZ ();
				gotoBeachRet ();
#endif
			} else {
				if (continue_after_jump) {
					recurseAt (op.jump);
					recurseAt (op.fail);
				} else {
					// This code seems to break #1519
					if (anal->opt.eobjmp) {
#if JMP_IS_EOB
						if (!overlapped) {
							bb->jump = op.jump;
							bb->fail = UT64_MAX;
						}
						FITFCNSZ ();
						return R_ANAL_RET_END;
#else
						// hardcoded jmp size // must be checked at the end wtf?
						// always fitfcnsz and retend
						if (r_anal_fcn_is_in_offset (fcn, op.jump)) {
							/* jump inside the same function */
							FITFCNSZ ();
							return R_ANAL_RET_END;
#if JMP_IS_EOB_RANGE > 0
						} else {
							if (op.jump < addr - JMP_IS_EOB_RANGE && op.jump < addr) {
								gotoBeach (R_ANAL_RET_END);
							}
							if (op.jump > addr + JMP_IS_EOB_RANGE) {
								gotoBeach (R_ANAL_RET_END);
							}
#endif
						}
#endif
					} else {
						/* if not eobjmp. a jump will break the function if jumps before the beginning of the function */
						if (op.jump < fcn->addr) {
							if (!overlapped) {
								bb->jump = op.jump;
								bb->fail = UT64_MAX;
							}
							FITFCNSZ ();
							return R_ANAL_RET_END;
						}
					}
				}
			}
			break;
		case R_ANAL_OP_TYPE_CJMP:
			if (anal->opt.cjmpref) {
				(void) r_anal_fcn_xref_add (anal, fcn,
					op.addr, op.jump, R_ANAL_REF_TYPE_CODE);
			}
			if (!overlapped) {
				bb->jump = op.jump;
				bb->fail = op.fail;
			}
			if (continue_after_jump) {
				recurseAt (op.jump);
				recurseAt (op.fail);
			} else {
				// This code seems to break #1519
				if (anal->opt.eobjmp) {
#if JMP_IS_EOB
					if (!overlapped) {
						bb->jump = op.jump;
						bb->fail = UT64_MAX;
					}
					FITFCNSZ ();
					recurseAt (op.jump);
					recurseAt (op.fail);
					return R_ANAL_RET_END;
#else
					// hardcoded jmp size // must be checked at the end wtf?
					// always fitfcnsz and retend
					if (op.jump > fcn->addr + JMP_IS_EOB_RANGE) {
						recurseAt (op.fail);
						/* jump inside the same function */
						FITFCNSZ ();
						return R_ANAL_RET_END;
#if JMP_IS_EOB_RANGE > 0
					} else {
						if (op.jump < addr - JMP_IS_EOB_RANGE && op.jump < addr) {
							gotoBeach (R_ANAL_RET_END);
						}
						if (op.jump > addr + JMP_IS_EOB_RANGE) {
							gotoBeach (R_ANAL_RET_END);
						}
#endif
					}
#endif
					recurseAt (op.jump);
					recurseAt (op.fail);
				} else {
					/* if not eobjmp. a jump will break the function if jumps before the beginning of the function */
					recurseAt (op.jump);
					recurseAt (op.fail);
					if (op.jump < fcn->addr) {
						if (!overlapped) {
							bb->jump = op.jump;
							bb->fail = UT64_MAX;
						}
						FITFCNSZ ();
						return R_ANAL_RET_END;
					}
				}
			}

			// XXX breaks mips analysis too !op.delay
			// this will be all x86, arm (at least)
			// without which the analysis is really slow,
			// presumably because each opcode would get revisited
			// (and already covered by a bb) many times
			gotoBeachRet ();
			// For some reason, branch delayed code (MIPS) needs to continue
			break;
		case R_ANAL_OP_TYPE_UCALL:
		case R_ANAL_OP_TYPE_RCALL:
		case R_ANAL_OP_TYPE_ICALL:
		case R_ANAL_OP_TYPE_IRCALL:
			/* call [dst] */
			if (op.ptr != UT64_MAX && r_anal_noreturn_at (anal, op.ptr)) {
				FITFCNSZ ();
				r_anal_op_fini (&op);
				return R_ANAL_RET_END;
			}
			// XXX: this is TYPE_MCALL or indirect-call
			(void) r_anal_fcn_xref_add (anal, fcn, op.addr, op.ptr, R_ANAL_REF_TYPE_CALL);
			break;
		case R_ANAL_OP_TYPE_CCALL:
		case R_ANAL_OP_TYPE_CALL:
			/* call dst */
			if (r_anal_noreturn_at (anal, op.jump)) {
				FITFCNSZ ();
				r_anal_op_fini (&op);
				return R_ANAL_RET_END;
			}
			(void) r_anal_fcn_xref_add (anal, fcn, op.addr, op.jump, R_ANAL_REF_TYPE_CALL);
#if CALL_IS_EOB
			recurseAt (op.jump);
			recurseAt (op.fail);
			gotoBeach (R_ANAL_RET_NEW);
#endif
			break;
		case R_ANAL_OP_TYPE_MJMP:
		case R_ANAL_OP_TYPE_UJMP:
		case R_ANAL_OP_TYPE_RJMP:
		case R_ANAL_OP_TYPE_IJMP:
		case R_ANAL_OP_TYPE_IRJMP:
			// switch statement
			if (anal->opt.jmptbl) {
				if (fcn->refs->tail) {
					RAnalRef *last_ref = fcn->refs->tail->data;
					last_ref->type = R_ANAL_REF_TYPE_NULL;
				}
				if (op.ptr != UT64_MAX) {       // direct jump
					ret = try_walkthrough_jmptbl (anal, fcn, depth, addr + idx, op.ptr, ret);

				} else {        // indirect jump: table pointer is unknown
					if (op.src[0] && op.src[0]->reg) {
						ut64 ptr = search_reg_val (anal, buf, idx, addr, op.src[0]->reg->name);
						if (ptr && ptr != UT64_MAX) {
							ret = try_walkthrough_jmptbl (anal, fcn, depth, addr + idx, ptr, ret);
						}
					}
				}
				walk_switch (anal, fcn, op.addr, op.addr + op.size);
			}
#if 0
			if (anal->cur) {
				/* if UJMP is in .plt section just skip it */
				RBinSection *s = anal->binb.get_vsect_at (anal->binb.bin, addr);
				if (s && s->name[0]) {
					bool in_plt = strstr (s->name, ".plt") != NULL;
					if (!in_plt && strstr (s->name, "_stubs") != NULL) {
						/* for mach0 */
						in_plt = true;
					}
					if (anal->cur->arch && strstr (anal->cur->arch, "arm")) {
						if (anal->bits == 64) {
							if (!in_plt) {
								goto river;
							}
						}
					} else {
						if (in_plt && !strstr (s->name, "_stubs")) {
							goto river;
						}
					}
				}
			}
#endif
			FITFCNSZ ();
			r_anal_op_fini (&op);
			return R_ANAL_RET_END;
#if 0
river:
			break;
#endif
		/* fallthru */
		case R_ANAL_OP_TYPE_PUSH:
			last_is_push = true;
			last_push_addr = op.val;
			if (anal->iob.is_valid_offset (anal->iob.io, op.val, 1)) {
				(void) r_anal_fcn_xref_add (anal, fcn, op.addr, op.val, R_ANAL_REF_TYPE_DATA);
			}
			break;
		case R_ANAL_OP_TYPE_RET:
			if (op.family == R_ANAL_OP_FAMILY_PRIV) {
				fcn->type = R_ANAL_FCN_TYPE_INT;
			}
			if (last_is_push && anal->opt.pushret) {
				op.type = R_ANAL_OP_TYPE_JMP;
				op.jump = last_push_addr;
				bb->jump = op.jump;
				recurseAt (op.jump);
				gotoBeachRet ();
			} else {
				if (!op.cond) {
					VERBOSE_ANAL eprintf("RET 0x%08"PFMT64x ". %d %d %d\n",
					addr + delay.un_idx - oplen, overlapped,
					bb->size, r_anal_fcn_size (fcn));
					FITFCNSZ ();
					r_anal_op_fini (&op);
					return R_ANAL_RET_END;
				}
			}
			break;
		}
		if (op.type != R_ANAL_OP_TYPE_PUSH) {
			last_is_push = false;
		}
	}
beach:
	r_anal_op_fini (&op);
	FITFCNSZ ();
	return ret;
}

static int check_preludes(ut8 *buf, ut16 bufsz) {
	if (bufsz < 10) {
		return false;
	}
	if (!memcmp (buf, (const ut8 *) "\x55\x89\xe5", 3)) {
		return true;
	} else if (!memcmp (buf, (const ut8 *) "\x55\x8b\xec", 3)) {
		return true;
	} else if (!memcmp (buf, (const ut8 *) "\x8b\xff", 2)) {
		return true;
	} else if (!memcmp (buf, (const ut8 *) "\x55\x48\x89\xe5", 4)) {
		return true;
	} else if (!memcmp (buf, (const ut8 *) "\x55\x48\x8b\xec", 4)) {
		return true;
	}
	return false;
}

R_API bool r_anal_check_fcn(RAnal *anal, ut8 *buf, ut16 bufsz, ut64 addr, ut64 low, ut64 high) {
	RAnalOp op = {
		0
	};
	int i, oplen, opcnt = 0, pushcnt = 0, movcnt = 0, brcnt = 0;
	if (check_preludes (buf, bufsz)) {
		return true;
	}
	for (i = 0; i < bufsz && opcnt < 10; i += oplen, opcnt++) {
		r_anal_op_fini (&op);
		if ((oplen = r_anal_op (anal, &op, addr + i, buf + i, bufsz - i)) < 1) {
			return false;
		}
		switch (op.type) {
		case R_ANAL_OP_TYPE_PUSH:
		case R_ANAL_OP_TYPE_UPUSH:
			pushcnt++;
			break;
		case R_ANAL_OP_TYPE_MOV:
		case R_ANAL_OP_TYPE_CMOV:
			movcnt++;
			break;
		case R_ANAL_OP_TYPE_JMP:
		case R_ANAL_OP_TYPE_CJMP:
		case R_ANAL_OP_TYPE_CALL:
			if (op.jump < low || op.jump >= high) {
				return false;
			}
			brcnt++;
			break;
		case R_ANAL_OP_TYPE_UNK:
			return false;
		}
	}
	return (pushcnt + movcnt + brcnt > 5);
}

static void fcnfit(RAnal *a, RAnalFunction *f) {
	// find next function
	RAnalFunction *next = r_anal_fcn_next (a, f->addr);
	if (next) {
		if ((f->addr + r_anal_fcn_size (f)) > next->addr) {
			r_anal_fcn_resize (f, (next->addr - f->addr));
		}
	}
}

R_API void r_anal_fcn_fit_overlaps(RAnal *anal, RAnalFunction *fcn) {
	if (fcn) {
		fcnfit (anal, fcn);
	} else {
		RAnalFunction *f;
		RListIter *iter;
		r_list_foreach (anal->fcns, iter, f) {
			fcnfit (anal, f);
		}
	}
}

R_API void r_anal_trim_jmprefs(RAnalFunction *fcn) {
	RAnalRef *ref;
	RListIter *iter;
	RListIter *tmp;
	r_list_foreach_safe (fcn->refs, iter, tmp, ref) {
		if (ref->type == R_ANAL_REF_TYPE_CODE && r_anal_fcn_is_in_offset (fcn, ref->addr)) {
			r_list_delete (fcn->refs, iter);
		}
	}
}

R_API int r_anal_fcn(RAnal *anal, RAnalFunction *fcn, ut64 addr, ut8 *buf, ut64 len, int reftype) {
	int ret;
	r_anal_fcn_set_size (fcn, 0);
	/* defines fcn. or loc. prefix */
	fcn->type = (reftype == R_ANAL_REF_TYPE_CODE)
	? R_ANAL_FCN_TYPE_LOC
		: R_ANAL_FCN_TYPE_FCN;
	if (fcn->addr == UT64_MAX) {
		fcn->addr = addr;
	}
	if (anal->cur && anal->cur->fcn) {
		int result = anal->cur->fcn (anal, fcn, addr, buf, len, reftype);
		if (anal->cur->custom_fn_anal) {
			return result;
		}
	}
	fcn->maxstack = 0;
	ret = fcn_recurse (anal, fcn, addr, buf, len, FCN_DEPTH);
	// update tinyrange for the function
	r_anal_fcn_update_tinyrange_bbs (fcn);

	if (ret == R_ANAL_RET_END && r_anal_fcn_size (fcn)) {   // cfg analysis completed
		RListIter *iter;
		RAnalBlock *bb;
		ut64 endaddr = fcn->addr;
		ut64 overlapped = -1;
		RAnalFunction *fcn1 = NULL;

		// set function size as length of continuous sequence of bbs
		r_list_sort (fcn->bbs, &cmpaddr);
		r_list_foreach (fcn->bbs, iter, bb) {
			if (endaddr == bb->addr) {
				endaddr += bb->size;
			} else if (endaddr < bb->addr &&
			bb->addr - endaddr <
			anal->opt.bbs_alignment &&
			!(bb->addr &
			(anal->opt.bbs_alignment - 1))) {
				endaddr = bb->addr + bb->size;
			} else {
				break;
			}
		}
#if !JAYRO_04
		r_anal_fcn_resize (fcn, endaddr - fcn->addr);

		// resize function if overlaps
		r_list_foreach (anal->fcns, iter, fcn1) {
			if (fcn1->addr >= (fcn->addr) && fcn1->addr < (fcn->addr + r_anal_fcn_size (fcn))) {
				if (overlapped > fcn1->addr) {
					overlapped = fcn1->addr;
				}
			}
		}
		if (overlapped != -1) {
			r_anal_fcn_resize (fcn, overlapped - fcn->addr);
		}
#endif
		r_anal_trim_jmprefs (fcn);
	}
	return ret;
}

// TODO: need to implement r_anal_fcn_remove(RAnal *anal, RAnalFunction *fcn);
R_API int r_anal_fcn_insert(RAnal *anal, RAnalFunction *fcn) {
	// RAnalFunction *f = r_anal_get_fcn_in (anal, fcn->addr, R_ANAL_FCN_TYPE_ROOT);
	RAnalFunction *f = r_anal_get_fcn_at (anal, fcn->addr, R_ANAL_FCN_TYPE_ROOT);
	if (f) {
		return false;
	}
#if USE_NEW_FCN_STORE
	r_listrange_add (anal->fcnstore, fcn);
	// HUH? store it here .. for backweird compatibility
#endif
	/* TODO: sdbization */
	r_list_append (anal->fcns, fcn);
	r_anal_fcn_tree_insert (&anal->fcn_tree, fcn);
	if (anal->cb.on_fcn_new) {
		anal->cb.on_fcn_new (anal, anal->user, fcn);
	}
	return true;
}

R_API int r_anal_fcn_add(RAnal *a, ut64 addr, ut64 size, const char *name, int type, RAnalDiff *diff) {
	int append = 0;
	RAnalFunction *fcn = r_anal_get_fcn_in (a, addr, R_ANAL_FCN_TYPE_ROOT);
	if (!fcn) {
		if (!(fcn = r_anal_fcn_new ())) {
			return false;
		}
		append = 1;
	}
	fcn->addr = addr;
	fcn->cc = r_str_const (r_anal_cc_default (a));
	fcn->bits = a->bits;
	r_anal_fcn_set_size (fcn, size);
	free (fcn->name);
	if (!name) {
		fcn->name = r_str_newf ("fcn.%08"PFMT64x, fcn->addr);
	} else {
		fcn->name = strdup (name);
	}
	fcn->type = type;
	if (diff) {
		fcn->diff->type = diff->type;
		fcn->diff->addr = diff->addr;
		R_FREE (fcn->diff->name);
		if (diff->name) {
			fcn->diff->name = strdup (diff->name);
		}
	}
#if FCN_SDB
	sdb_set (DB, sdb_fmt (0, "fcn.0x%08"PFMT64x, addr), "TODO", 0); // TODO: add more info here
#endif
	return append? r_anal_fcn_insert (a, fcn): true;
}

R_API int r_anal_fcn_del_locs(RAnal *anal, ut64 addr) {
	RListIter *iter, *iter2;
	RAnalFunction *fcn, *f = r_anal_get_fcn_in (anal, addr,
		R_ANAL_FCN_TYPE_ROOT);
#if USE_NEW_FCN_STORE
#warning TODO: r_anal_fcn_del_locs not implemented for newstore
#endif
	if (!f) {
		return false;
	}
	r_list_foreach_safe (anal->fcns, iter, iter2, fcn) {
		if (fcn->type != R_ANAL_FCN_TYPE_LOC) {
			continue;
		}
		if (r_anal_fcn_in (fcn, addr)) {
			r_anal_fcn_tree_delete (&anal->fcn_tree, fcn);
			r_list_delete (anal->fcns, iter);
		}
	}
	r_anal_fcn_del (anal, addr);
	return true;
}

R_API int r_anal_fcn_del(RAnal *a, ut64 addr) {
	if (addr == UT64_MAX) {
#if USE_NEW_FCN_STORE
		r_listrange_free (a->fcnstore);
		a->fcnstore = r_listrange_new ();
#else
		r_list_free (a->fcns);
		a->fcn_tree = NULL;
		if (!(a->fcns = r_anal_fcn_list_new ())) {
			return false;
		}
#endif
	} else {
#if USE_NEW_FCN_STORE
		// XXX: must only get the function if starting at 0?
		RAnalFunction *f = r_listrange_find_in_range (a->fcnstore, addr);
		if (f) {
			r_listrange_del (a->fcnstore, f);
		}
#else
		RAnalFunction *fcni;
		RListIter *iter, *iter_tmp;
		r_list_foreach_safe (a->fcns, iter, iter_tmp, fcni) {
			if (r_anal_fcn_in (fcni, addr)) {
				if (a->cb.on_fcn_delete) {
					a->cb.on_fcn_delete (a, a->user, fcni);
				}
				r_anal_fcn_tree_delete (&a->fcn_tree, fcni);
				r_list_delete (a->fcns, iter);
			}
		}
#endif
	}
	return true;
}

R_API RAnalFunction *r_anal_get_fcn_in(RAnal *anal, ut64 addr, int type) {
#if USE_NEW_FCN_STORE
	// TODO: type is ignored here? wtf.. we need more work on fcnstore
	// if (root) return r_listrange_find_root (anal->fcnstore, addr);
	return r_listrange_find_in_range (anal->fcnstore, addr);
#else

# if 0
  // Linear scan
	RAnalFunction *fcn, *ret = NULL;
	RListIter *iter;
	if (type == R_ANAL_FCN_TYPE_ROOT) {
		r_list_foreach (anal->fcns, iter, fcn) {
			if (addr == fcn->addr) {
				return fcn;
			}
		}
		return NULL;
	}
	r_list_foreach (anal->fcns, iter, fcn) {
		if (!type || (fcn && fcn->type & type)) {
			if (r_tinyrange_in (&fcn->bbr, addr) || fcn->addr == addr) {
				ret = fcn;
				break;
			}
		}
	}
	return ret;

# else
  // Interval tree query
	RAnalFunction *fcn;
	FcnTreeIter it;
	if (type == R_ANAL_FCN_TYPE_ROOT) {
		return _fcn_tree_find_addr (anal->fcn_tree, addr);
	}
	fcn_tree_foreach_intersect (anal->fcn_tree, it, fcn, addr, addr + 1) {
		if (!type || (fcn && fcn->type & type)) {
			if (r_tinyrange_in (&fcn->bbr, addr) || fcn->addr == addr) {
				return fcn;
			}
		}
	}
	return NULL;
# endif
#endif // USE_NEW_FCN_STORE
}

R_API bool r_anal_fcn_in(RAnalFunction *fcn, ut64 addr) {
	return fcn? r_tinyrange_in (&fcn->bbr, addr): false;
}

R_API RAnalFunction *r_anal_get_fcn_in_bounds(RAnal *anal, ut64 addr, int type) {
#if USE_NEW_FCN_STORE
#warning TODO: r_anal_get_fcn_in_bounds
	// TODO: type is ignored here? wtf.. we need more work on fcnstore
	// if (root) return r_listrange_find_root (anal->fcnstore, addr);
	return r_listrange_find_in_range (anal->fcnstore, addr);
#else
	RAnalFunction *fcn, *ret = NULL;
	RListIter *iter;
	if (type == R_ANAL_FCN_TYPE_ROOT) {
		r_list_foreach (anal->fcns, iter, fcn) {
			if (addr == fcn->addr) {
				return fcn;
			}
		}
		return NULL;
	}
	r_list_foreach (anal->fcns, iter, fcn) {
		if (!type || (fcn && fcn->type & type)) {
			if (r_anal_fcn_in (fcn, addr)) {
				return fcn;
			}
		}
	}
	return ret;
#endif
}

R_API RAnalFunction *r_anal_fcn_find_name(RAnal *anal, const char *name) {
	RAnalFunction *fcn = NULL;
	RListIter *iter;
	r_list_foreach (anal->fcns, iter, fcn) {
		if (!strcmp (name, fcn->name)) {
			return fcn;
		}
	}
	return NULL;
}

/* rename RAnalFunctionBB.add() */
R_API int r_anal_fcn_add_bb(RAnal *anal, RAnalFunction *fcn, ut64 addr, ut64 size, ut64 jump, ut64 fail, int type, RAnalDiff *diff) {
	RAnalBlock *bb = NULL, *bbi;
	RListIter *iter;
	bool mid = false;
	st64 n;

	r_list_foreach (fcn->bbs, iter, bbi) {
		if (addr == bbi->addr) {
			bb = bbi;
			mid = false;
			break;
		} else if ((addr > bbi->addr) && (addr < bbi->addr + bbi->size)) {
			mid = true;
		}
	}
	if (mid) {
		// eprintf ("Basic Block overlaps another one that should be shrinked\n");
		if (bbi) {
			/* shrink overlapped basic block */
			bbi->size = addr - (bbi->addr);
			r_anal_fcn_update_tinyrange_bbs (fcn);
		}
	}
	if (!bb) {
		bb = appendBasicBlock (anal, fcn, addr);
		if (!bb) {
			eprintf ("appendBasicBlock failed\n");
			return false;
		}
	}
	bb->addr = addr;
	bb->size = size;
	bb->jump = jump;
	bb->fail = fail;
	bb->type = type;
	if (diff) {
		if (!bb->diff) {
			bb->diff = r_anal_diff_new ();
		}
		if (bb->diff) {
			bb->diff->type = diff->type;
			bb->diff->addr = diff->addr;
			if (diff->name) {
				R_FREE (bb->diff->name);
				bb->diff->name = strdup (diff->name);
			}
		}
	}
	r_anal_fcn_update_tinyrange_bbs (fcn);
	n = bb->addr + bb->size - fcn->addr;
	if (n >= 0 && r_anal_fcn_size (fcn) < n) {
		// If fcn is in anal->fcn_tree (which reflects anal->fcns), update fcn_tree because fcn->_size has changed.
		if (r_anal_fcn_tree_delete (&anal->fcn_tree, fcn)) {
			r_anal_fcn_set_size (fcn, n);
			r_anal_fcn_tree_insert (&anal->fcn_tree, fcn);
		}
	}
	return true;
}

// TODO: rename fcn_bb_split()
// bb seems to be ignored
R_API int r_anal_fcn_split_bb(RAnal *anal, RAnalFunction *fcn, RAnalBlock *bb, ut64 addr) {
	RAnalBlock *bbi;
	RListIter *iter;
	if (addr == UT64_MAX) {
		return 0;
	}
	r_list_foreach (fcn->bbs, iter, bbi) {
		if (addr == bbi->addr) {
			return R_ANAL_RET_DUP;
		}
		if (addr > bbi->addr && addr < bbi->addr + bbi->size) {
			int new_bbi_instr, i;
			bb = appendBasicBlock (anal, fcn, addr);
			bb->size = bbi->addr + bbi->size - addr;
			bb->jump = bbi->jump;
			bb->fail = bbi->fail;
			bb->conditional = bbi->conditional;
			bbi->size = addr - bbi->addr;
			bbi->jump = addr;
			bbi->fail = -1;
			bbi->conditional = false;
			if (bbi->type & R_ANAL_BB_TYPE_HEAD) {
				bb->type = bbi->type ^ R_ANAL_BB_TYPE_HEAD;
				bbi->type = R_ANAL_BB_TYPE_HEAD;
			} else {
				bb->type = bbi->type;
				bbi->type = R_ANAL_BB_TYPE_BODY;
			}
			// recalculate offset of instructions in both bb and bbi
			i = 0;
			while (i < bbi->ninstr && r_anal_bb_offset_inst (bbi, i) < bbi->size) {
				i++;
			}
			new_bbi_instr = i;
			if (bb->addr - bbi->addr == r_anal_bb_offset_inst (bbi, i)) {
				bb->ninstr = 0;
				while (i < bbi->ninstr) {
					ut16 off_op = r_anal_bb_offset_inst (bbi, i);
					if (off_op >= bbi->size + bb->size) {
						break;
					}
					r_anal_bb_set_offset (bb, bb->ninstr, off_op - bbi->size);
					bb->ninstr++;
					i++;
				}
			}
			bbi->ninstr = new_bbi_instr;
			return R_ANAL_RET_END;
		}
	}
	return R_ANAL_RET_NEW;
}

// TODO: rename fcn_bb_overlap()
R_API int r_anal_fcn_bb_overlaps(RAnalFunction *fcn, RAnalBlock *bb) {
	RAnalBlock *bbi;
	RListIter *iter;
	r_list_foreach (fcn->bbs, iter, bbi) {
		if (bb->addr + bb->size > bbi->addr && bb->addr + bb->size <= bbi->addr + bbi->size) {
			bb->size = bbi->addr - bb->addr;
			bb->jump = bbi->addr;
			bb->fail = -1;
			bb->conditional = false;
			if (bbi->type & R_ANAL_BB_TYPE_HEAD) {
				bb->type = R_ANAL_BB_TYPE_HEAD;
				bbi->type = bbi->type ^ R_ANAL_BB_TYPE_HEAD;
			} else {
				bb->type = R_ANAL_BB_TYPE_BODY;
			}
			r_list_append (fcn->bbs, bb);
			return R_ANAL_RET_END;
		}
	}
	return R_ANAL_RET_NEW;
}

R_API int r_anal_fcn_loops(RAnalFunction *fcn) {
	RListIter *iter;
	RAnalBlock *bb;
	ut32 loops = 0;
	r_list_foreach (fcn->bbs, iter, bb) {
		if (bb->jump != UT64_MAX && bb->jump < bb->addr) {
			loops ++;
		}
		if (bb->fail != UT64_MAX && bb->fail < bb->addr) {
			loops ++;
		}
	}
	return loops;
}

R_API int r_anal_fcn_cc(RAnalFunction *fcn) {
/*
        CC = E - N + 2P
        E = the number of edges of the graph.
        N = the number of nodes of the graph.
        P = the number of connected components (exit nodes).
 */
	int E = 0, N = 0, P = 0;
	RListIter *iter;
	RAnalBlock *bb;

	r_list_foreach (fcn->bbs, iter, bb) {
		N++; // nodes
		if (bb->jump == UT64_MAX) {
			P++; // exit nodes
		} else {
			E++; // edges
			if (bb->fail != UT64_MAX) {
				E++;
			}
		}
	}
	return E - N + 2; // (2 * P);
}

R_API char *r_anal_fcn_to_string(RAnal *a, RAnalFunction *fs) {
	return NULL;
}

// TODO: This function is not fully implemented
/* set function signature from string */
R_API int r_anal_str_to_fcn(RAnal *a, RAnalFunction *f, const char *sig) {
	int length = 0;
	if (!a || !f || !sig) {
		eprintf ("r_anal_str_to_fcn: No function received\n");
		return false;
	}
	length = strlen (sig) + 10;
	/* Add 'function' keyword */
	char *str = calloc (1, length);
	if (!str) {
		eprintf ("Cannot allocate %d byte(s)\n", length);
		return false;
	}
	strcpy (str, "function ");
	strcat (str, sig);

	/* TODO: improve arguments parsing */
	/* TODO: implement parser */
	/* TODO: simplify this complex api usage */

	free (str);
	return true;
}

R_API RAnalFunction *r_anal_get_fcn_at(RAnal *anal, ut64 addr, int type) {
#if USE_NEW_FCN_STORE
	// TODO: type is ignored here? wtf.. we need more work on fcnstore
	// if (root) return r_listrange_find_root (anal->fcnstore, addr);
	return r_listrange_find_root (anal->fcnstore, addr);
#else
	RAnalFunction *fcn, *ret = NULL;
	RListIter *iter;
	if (type == R_ANAL_FCN_TYPE_ROOT) {
		r_list_foreach (anal->fcns, iter, fcn) {
			if (addr == fcn->addr) {
				return fcn;
			}
		}
		return NULL;
	}
	r_list_foreach (anal->fcns, iter, fcn) {
		if (!type || (fcn->type & type)) {
			if (addr == fcn->addr) {
				ret = fcn;
			}
		}
	}
	return ret;
#endif
}

R_API RAnalFunction *r_anal_fcn_next(RAnal *anal, ut64 addr) {
	RAnalFunction *fcni;
	RListIter *iter;
	RAnalFunction *closer = NULL;
	r_list_foreach (anal->fcns, iter, fcni) {
		// if (fcni->addr == addr)
		if (fcni->addr > addr && (!closer || fcni->addr < closer->addr)) {
			closer = fcni;
		}
	}
	return closer;
}

/* getters */
#if FCN_OLD
R_API RList *r_anal_fcn_get_refs(RAnalFunction *fcn) {
	return fcn->refs;
}
R_API RList *r_anal_fcn_get_xrefs(RAnalFunction *fcn) {
	return fcn->xrefs;
}
#endif

R_API RList *r_anal_fcn_get_bbs(RAnalFunction *anal) {
	// avoid received to free this thing
	// anal->bbs->rc++;
	anal->bbs->free = NULL;
	return anal->bbs;
}

R_API int r_anal_fcn_is_in_offset(RAnalFunction *fcn, ut64 addr) {
	if (r_list_empty (fcn->bbs)) {
		// r_anal_fcn_size (fcn);
		return addr >= fcn->addr && addr < fcn->addr + fcn->_size;
	}
	if (r_anal_fcn_in (fcn, addr)) {
		return true;
	}
	return false;
}

R_API int r_anal_fcn_count(RAnal *anal, ut64 from, ut64 to) {
	int n = 0;
	RAnalFunction *fcni;
	RListIter *iter;
	r_list_foreach (anal->fcns, iter, fcni) {
		if (fcni->addr >= from && fcni->addr < to) {
			n++;
		}
	}
	return n;
}

/* return the basic block in fcn found at the given address.
 * NULL is returned if such basic block doesn't exist. */
R_API RAnalBlock *r_anal_fcn_bbget(RAnalFunction *fcn, ut64 addr) {
	if (!fcn || addr == UT64_MAX) {
		return NULL;
	}
#if USE_SDB_CACHE
	return sdb_ptr_get (HB, sdb_fmt (0, SDB_KEY_BB, fcn->addr, addr), NULL);
#else
	RListIter *iter;
	RAnalBlock *bb;
	r_list_foreach (fcn->bbs, iter, bb) {
		if (bb->addr == addr) {
			return bb;
		}
	}
	return NULL;
#endif
}

R_API bool r_anal_fcn_bbadd(RAnalFunction *fcn, RAnalBlock *bb) {
#if USE_SDB_CACHE
	return sdb_ptr_set (HB, sdb_fmt (0, SDB_KEY_BB, fcn->addr, bb->addr), bb, NULL);
#endif
	r_list_append (fcn->bbs, bb);
	return true;
}

/* directly set the size of the function */
R_API void r_anal_fcn_set_size(RAnalFunction *fcn, ut32 size) {
	if (fcn) {
		fcn->_size = size;
	}
}

/* returns the size of the function.
 * IMPORTANT: this will change, one day, because it doesn't have much sense */
R_API ut32 r_anal_fcn_size(const RAnalFunction *fcn) {
	return fcn? fcn->_size: 0;
}

/* return the "real" size of the function, that is the sum of the size of the
 * basicblocks this function is composed of.
 * IMPORTANT: this will become, one day, the only size of a function */
R_API ut32 r_anal_fcn_realsize(const RAnalFunction *fcn) {
	RListIter *iter, *fiter;
	RAnalBlock *bb;
	RAnalFunction *f;
	ut32 sz = 0;
	r_list_foreach (fcn->bbs, iter, bb) {
		sz += bb->size;
	}
	r_list_foreach (fcn->fcn_locs, fiter, f) {
		r_list_foreach (f->bbs, iter, bb) {
			sz += bb->size;
		}
	}
	return sz;
}

// continious function size without loc.*
R_API ut32 r_anal_fcn_contsize(const RAnalFunction *fcn) {
	RListIter *iter;
	RAnalBlock *bb;
	ut32 sz = 0;
	r_list_foreach (fcn->bbs, iter, bb) {
		/* TODO: this if is an ugly hack and should be removed when r2 will be
		* able to handle BBs that comes before the function emtry point.
		* Another way to remove this is to throw away BBs before the function
		* entry point at the analysis time in the r_anal_fcn.   */
		if (bb->addr >= fcn->addr) {
			sz += bb->size;
		}
	}
	return sz;
}

// compute the cyclomatic cost
R_API ut32 r_anal_fcn_cost(RAnal *anal, RAnalFunction *fcn) {
	RListIter *iter;
	RAnalBlock *bb;
	ut32 totalCycles = 0;
	if (!fcn) {
		return 0;
	}
	r_list_foreach (fcn->bbs, iter, bb) {
		RAnalOp op;
		ut64 at, end = bb->addr + bb->size;
		ut8 *buf = malloc (bb->size);
		anal->iob.read_at (anal->iob.io, bb->addr, (ut8 *) buf, bb->size);
		int idx = 0;
		for (at = bb->addr; at < end;) {
			memset (&op, 0, sizeof (op));
			(void) r_anal_op (anal, &op, at, buf + idx, bb->size - idx);
			if (op.size < 1) {
				op.size = 1;
			}
			idx += op.size;
			at += op.size;
			totalCycles += op.cycles;
		}
		free (buf);
	}
	return totalCycles;
}

R_API int r_anal_fcn_count_edges(RAnalFunction *fcn, int *ebbs) {
	RListIter *iter;
	RAnalBlock *bb;
	int edges = 0;
	if (ebbs) {
		*ebbs = 0;
	}
	r_list_foreach (fcn->bbs, iter, bb) {
		if (ebbs && bb->jump == UT64_MAX && bb->fail == UT64_MAX) {
			*ebbs = *ebbs + 1;
		} else {
			if (bb->jump != UT64_MAX) {
				edges ++;
			}
			if (bb->fail != UT64_MAX) {
				edges ++;
			}
		}
	}
	return edges;
}

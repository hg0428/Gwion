#include <stdlib.h>
#include <string.h>
#include "gwion_util.h"
#include "gwion_ast.h"
#include "gwion_env.h"
#include "vm.h"
#include "gwion.h"
#include "instr.h"
#include "object.h"
#include "operator.h"
#include "import.h"
#include "emit.h"
#include "traverse.h"
#include "template.h"
#include "parse.h"
#include "specialid.h"

#include "gwi.h"
#include "gack.h"

#undef insert_symbol
ANN void exception(const VM_Shred shred, const m_str c) {
  gw_err("%s: shred[id=%" UINT_F ":%s], PC=[%" UINT_F "]\n",
          c, shred->tick->xid, shred->info->name, shred->pc - 1);
  vm_shred_exit(shred);
}

M_Object new_object(MemPool p, const VM_Shred shred, const Type t) {
  const M_Object a = mp_calloc(p, M_Object);
  a->ref = 1;
  a->type_ref = t;
  if(t->nspc) {
    a->vtable = &t->nspc->info->vtable;
    if(t->nspc->info->offset)
      a->data = (m_bit*)_mp_calloc(p, t->nspc->info->offset);
  }
  if(shred)
    vector_add(&shred->gc, (vtype)a);
  return a;
}

M_Object new_string(MemPool p, const VM_Shred shred, const m_str str) {
  const M_Object o = new_object(p, shred, shred->info->vm->gwion->type[et_string]);
  STRING(o) = s_name(insert_symbol(shred->info->vm->gwion->st, str));
  return o;
}

M_Object new_string2(const struct Gwion_ *gwion, const VM_Shred shred, const m_str str) {
  const M_Object o = new_object(gwion->mp, shred, gwion->type[et_string]);
  STRING(o) = str;
  return o;
}

ANN static void handle_dtor(const M_Object o, const VM_Shred shred) {
  const VM_Shred sh = new_vm_shred(shred->info->mp, o->type_ref->nspc->dtor);
  ADD_REF(o->type_ref->nspc->dtor);
  sh->base = shred->base;
  *(M_Object*)sh->mem = o;
  vm_add_shred(shred->info->vm, sh);
  ++sh->info->me->ref;
}

__attribute__((hot))
ANN void __release(const M_Object o, const VM_Shred shred) {
  MemPool p = shred->info->mp;
  Type t = o->type_ref;
  do {
    if(GET_FLAG(t, nonnull))
      t = t->e->parent;
    if(!t->nspc)
      continue;
    struct scope_iter iter = { t->nspc->info->value, 0, 0 };\
    Value v;
    while(scope_iter(&iter, &v) > 0) {
      if(!GET_FLAG(v, static) && !GET_FLAG(v, pure) &&
          isa(v->type, shred->info->vm->gwion->type[et_object]) > 0)
        release(*(M_Object*)(o->data + v->from->offset), shred);
    }
    if(GET_FLAG(t, dtor) && t->nspc->dtor) {
      if(GET_FLAG(t->nspc->dtor, builtin))
        ((f_xtor)t->nspc->dtor->native_func)(o, NULL, shred);
      else {
        o->type_ref = t;
        handle_dtor(o, shred);
        return;
      }
    }
  } while((t = t->e->parent));
  free_object(p, o);
}

ANN void free_object(MemPool p, const M_Object o) {
  if(o->type_ref->nspc && o->type_ref->nspc->info->offset)
    mp_free2(p, o->type_ref->nspc->info->offset, o->data);
  mp_free(p, M_Object, o);
}

#define describe_logical(name, op)               \
static INSTR(name##Object) {\
  POP_REG(shred, SZ_INT);                        \
  const M_Object lhs = *(M_Object*)REG(-SZ_INT); \
  const M_Object rhs = *(M_Object*)REG(0);       \
  *(m_uint*)REG(-SZ_INT) = (lhs op rhs);         \
  release(lhs, shred);                           \
  release(rhs, shred);                           \
}

describe_logical(Eq,  ==)
describe_logical(Neq, !=)

static inline m_bool nonnull_check(const Type l, const Type r) {
  return !GET_FLAG(l, nonnull) && GET_FLAG(r, nonnull);
}

static inline Type check_nonnull(const Env env, const Type l, const Type r,
      const m_str action, const loc_t pos) {
  if(GET_FLAG(r, nonnull)) {
    if(isa(l, env->gwion->type[et_null]) > 0)
      ERR_N(pos, _("can't %s '%s' to '%s'"), action, l->name, r->name);
    if(isa(l, r) < 0)
      ERR_N(pos, _("can't %s '%s' to '%s'"), action, l->name, r->name);
    return r->e->parent;
  }
  if(l != env->gwion->type[et_null] && isa(l, r) < 0)
    ERR_N(pos, _("can't %s '%s' to '%s'"), action, l->name, r->name);
  return r;
}

static OP_CHECK(at_object) {
  const Exp_Binary* bin = (Exp_Binary*)data;
  const Type l = bin->lhs->type;
  const Type r = bin->rhs->type;
  if(opck_rassign(env, data, mut) == env->gwion->type[et_null])
    return env->gwion->type[et_null];
  if(check_nonnull(env, l, r, "assign", exp_self(bin)->pos) == env->gwion->type[et_null])
    return env->gwion->type[et_null];
  if(bin->rhs->exp_type == ae_exp_decl) {
    SET_FLAG(bin->rhs->d.exp_decl.td, ref);
    SET_FLAG(bin->rhs->d.exp_decl.list->self->value, ref);
  }
  bin->rhs->emit_var = 1;
  return r;
}

static OP_EMIT(opem_at_object) {
  const Exp_Binary* bin = (Exp_Binary*)data;
  const Type l = bin->lhs->type;
  const Type r = bin->rhs->type;
  if(nonnull_check(l, r)) {
    const Instr instr = emit_add_instr(emit, GWOP_EXCEPT);
    instr->m_val = SZ_INT;
  }
  return emit_add_instr(emit, ObjectAssign);
}

#define STR_FORCE ":force"
#define STRLEN_FORCE strlen(STR_FORCE)

static inline Type new_force_type(MemPool p, const Type t, const Symbol sym) {
  const Type ret = type_copy(p, t);
  if(ret->nspc)
    ADD_REF(ret->nspc)
  ret->name = s_name(sym);
  ret->flag = t->flag | ae_flag_force;
  nspc_add_type_front(t->e->owner, sym, ret);
  return ret;
 }

static Type get_force_type(const Env env, const Type t) {
  const size_t len = strlen(t->name);
  char name[len + STRLEN_FORCE + 2];
  strcpy(name, t->name);
  strcpy(name + len, STR_FORCE);
  const Symbol sym = insert_symbol(env->gwion->st, name);
  return nspc_lookup_type1(t->e->owner, sym) ?: new_force_type(env->gwion->mp, t, sym);
}

static OP_CHECK(opck_object_cast) {
  const Exp_Cast* cast = (Exp_Cast*)data;
  const Type l = cast->exp->type;
  const Type r = exp_self(cast)->type;
  if(check_nonnull(env, l, r, "cast", exp_self(cast)->pos) == env->gwion->type[et_null])
    return env->gwion->type[et_null];
  return get_force_type(env, r);
}

static OP_EMIT(opem_object_cast) {
  const Exp_Cast* cast = (Exp_Cast*)data;
  const Type l = cast->exp->type;
  const Type r = exp_self(cast)->type;
  if(nonnull_check(l, r))
    emit_add_instr(emit, GWOP_EXCEPT);
  return (Instr)GW_OK;
}

static OP_CHECK(opck_implicit_null2obj) {
  const struct Implicit* imp = (struct Implicit*)data;
  const Type l = imp->e->type;
  const Type r = imp->t;
  if(check_nonnull(env, l, r, "implicitly cast", imp->e->pos) == env->gwion->type[et_null])
    return env->gwion->type[et_null];
  imp->e->cast_to = r;
  return imp->t;
}

static OP_EMIT(opem_implicit_null2obj) {
  const struct Implicit* imp = (struct Implicit*)data;
  const Type l = imp->e->type;
  const Type r = imp->t;
  if(nonnull_check(l, r))
    emit_add_instr(emit, GWOP_EXCEPT);
  return (Instr)GW_OK;
}

static ID_CHECK(check_this) {
  if(!env->class_def)
    ERR_O(exp_self(prim)->pos, _("keyword 'this' can be used only inside class definition..."))
  if(env->func && !GET_FLAG(env->func, member))
    ERR_O(exp_self(prim)->pos, _("keyword 'this' cannot be used inside static functions..."))
  return env->class_def;
}

static GACK(gack_object) {
  INTERP_PRINTF("%p", *(M_Object*)VALUE);
}

ANN static Type scan_class(const Env env, const Type t, const Type_Decl* td);

static OP_CHECK(opck_object_scan) {
  struct TemplateScan *ts = (struct TemplateScan*)data;
  if(ts->td->types)
    return scan_class(env, ts->t, ts->td);
  ERR_O(td_pos(ts->td), _("you must provide template types for type '%s'"), ts->t->name)
}

static const f_instr dotstatic[]  = { DotStatic, DotStatic2, DotStatic3, RegPushImm };
ANN Instr emit_kind(Emitter emit, const m_uint size, const uint addr, const f_instr func[]);
ANN static void emit_dot_static_data(const Emitter emit, const Value v, const uint emit_var) {
  const m_uint size = v->type->size;
  const Instr instr = emit_kind(emit, size, emit_var, dotstatic);
  instr->m_val = (m_uint)(v->from->owner->info->class_data + v->from->offset);
  instr->m_val2 = size;
}

static const f_instr regpushimm[] = { RegPushImm, RegPushImm2, RegPushImm3, RegPushImm4 };
ANN static void emit_dot_static_import_data(const Emitter emit, const Value v, const uint emit_addr) {
  if(v->d.ptr && GET_FLAG(v, builtin) && GET_FLAG(v, const)) {
    const m_uint size = v->type->size;
    const Instr instr = emit_kind(emit, size, emit_addr, regpushimm);
    instr->m_val = (m_uint)v->d.ptr;
    instr->m_val2 = size;
  } else
    emit_dot_static_data(emit, v, emit_addr);
}
static const f_instr dotmember[]  = { DotMember, DotMember2, DotMember3, DotMember4 };

ANN static void emit_member_func(const Emitter emit, const Exp_Dot* member) {
  const Func f = exp_self(member)->type->e->d.func;
  if(is_class(emit->gwion, member->t_base) || GET_FLAG(member->base->type, force)) {
    const Instr func_i = emit_add_instr(emit, f->code ? RegPushImm : PushStaticCode);
    func_i->m_val = (m_uint)f->code;
    return;
  }
  if(f->def->base->tmpl)
    emit_add_instr(emit, DotTmplVal);
  else {
    const Instr instr = emit_add_instr(emit, GET_FLAG(f, member) ? DotFunc : DotStaticFunc);
    instr->m_val = f->vt_index;
  }
  return;
}

ANN static inline void emit_member(const Emitter emit, const Value v, const uint emit_addr) {
  const m_uint size = v->type->size;
  const Instr instr = emit_kind(emit, size, emit_addr, dotmember);
  instr->m_val = v->from->offset;
  instr->m_val2 = size;
}

ANN m_bool not_from_owner_class(const Env env, const Type t, const Value v, const loc_t pos);
OP_CHECK(opck_object_dot) {
  const Exp_Dot *member = (Exp_Dot*)data;
  const m_str str = s_name(member->xid);
  const m_bool base_static = is_class(env->gwion, member->t_base);
  const Type the_base = base_static ? member->t_base->e->d.base_type : member->t_base;
  if(!the_base->nspc)
    ERR_O(member->base->pos,
          _("type '%s' does not have members - invalid use in dot expression of %s"),
          the_base->name, str)
  if(member->xid ==  insert_symbol(env->gwion->st, "this") && base_static)
    ERR_O(exp_self(member)->pos,
          _("keyword 'this' must be associated with object instance..."))
  const Value value = find_value(the_base, member->xid);
  if(!value) {
    env_err(env, exp_self(member)->pos,
          _("class '%s' has no member '%s'"), the_base->name, str);
    if(member->t_base->nspc)
      did_you_mean_type(the_base, str);
    return NULL;
  }
  CHECK_BO(not_from_owner_class(env, the_base, value, exp_self(member)->pos))
  if(!env->class_def || isa(env->class_def, value->from->owner_class) < 0) {
    if(GET_FLAG(value, private))
      ERR_O(exp_self(member)->pos,
          _("can't access private '%s' outside of class..."), value->name)
    else if(GET_FLAG(value, protect))
      exp_self(member)->meta = ae_meta_protect;
  }
  if(base_static && GET_FLAG(value, member))
    ERR_O(exp_self(member)->pos,
          _("cannot access member '%s.%s' without object instance..."),
          the_base->name, str)
  if(GET_FLAG(value, const) || GET_FLAG(value, enum))
    exp_self(member)->meta = ae_meta_value;
  return value->type;
}

OP_EMIT(opem_object_dot) {
  const Exp_Dot *member = (Exp_Dot*)data;
  const Value value = find_value(actual_type(emit->gwion, member->t_base), member->xid);
  if(!is_class(emit->gwion, member->t_base) && (GET_FLAG(value, member) ||
       (isa(exp_self(member)->type, emit->gwion->type[et_function]) > 0 &&
       !is_fptr(emit->gwion, exp_self(member)->type)))) {
    CHECK_BO(emit_exp(emit, member->base, 0))
    emit_except(emit, member->t_base);
  }
  if(isa(exp_self(member)->type, emit->gwion->type[et_function]) > 0 && !is_fptr(emit->gwion, exp_self(member)->type))
	  emit_member_func(emit, member);
  else (GET_FLAG(value, member) ? emit_member : emit_dot_static_import_data)(emit, value, exp_self(member)->emit_var);
  return (Instr)GW_OK;
}

GWION_IMPORT(object) {
  const Type t_object  = gwi_mk_type(gwi, "Object", SZ_INT, NULL);
  gwi_add_type(gwi, t_object);
  GWI_BB(gwi_gack(gwi, t_object, gack_object))
  SET_FLAG(t_object, checked); // should be set by gwi_add_type
  gwi->gwion->type[et_object] = t_object;

  const Type t_null  = gwi_mk_type(gwi, "@null",  SZ_INT, "Object");
  gwi->gwion->type[et_null] = t_null;
  GWI_BB(gwi_set_global_type(gwi, t_null, et_null))
  GWI_BB(gwi_oper_cond(gwi, "Object", BranchEqInt, BranchNeqInt))
  GWI_BB(gwi_oper_ini(gwi, "Object", "Object", NULL))
  GWI_BB(gwi_oper_add(gwi, at_object))
  GWI_BB(gwi_oper_emi(gwi, opem_at_object))
  GWI_BB(gwi_oper_end(gwi, "@=>", ObjectAssign))
  GWI_BB(gwi_oper_ini(gwi, "Object", "Object", "int"))
  GWI_BB(gwi_oper_end(gwi, "==",  EqObject))
  GWI_BB(gwi_oper_end(gwi, "!=", NeqObject))
  GWI_BB(gwi_oper_add(gwi, opck_object_cast))
  GWI_BB(gwi_oper_emi(gwi, opem_object_cast))
  GWI_BB(gwi_oper_end(gwi, "$", NULL))
  GWI_BB(gwi_oper_add(gwi, opck_implicit_null2obj))
  GWI_BB(gwi_oper_emi(gwi, opem_implicit_null2obj))
  GWI_BB(gwi_oper_end(gwi, "@implicit", NULL))
  GWI_BB(gwi_oper_ini(gwi, "@null", "Object", "int"))
  GWI_BB(gwi_oper_add(gwi, opck_implicit_null2obj))
  GWI_BB(gwi_oper_end(gwi, "@implicit", NULL))
  GWI_BB(gwi_oper_ini(gwi, NULL, "Object", "bool"))
  GWI_BB(gwi_oper_add(gwi, opck_unary_meta2))
  GWI_BB(gwi_oper_end(gwi, "!", IntNot))
  GWI_BB(gwi_oper_ini(gwi, "Object", NULL, NULL))
  GWI_BB(gwi_oper_add(gwi, opck_object_scan))
  GWI_BB(gwi_oper_end(gwi, "@scan", NULL))
  gwi_item_ini(gwi, "@null", "null");
  gwi_item_end(gwi, 0, NULL);
  struct SpecialId_ spid = { .ck=check_this, .exec=RegPushMem, .is_const=1 };
  gwi_specialid(gwi, "this", &spid);
  return GW_OK;
}
struct tmpl_info {
  const  Class_Def cdef;
  Type_List        call;
  struct Vector_   type;
  struct Vector_   size;
  uint8_t index;
};

ANN static inline size_t tmpl_set(struct tmpl_info* info, const Type t) {
  vector_add(&info->type, (vtype)t);
  const size_t len = strlen(t->name);
  vector_add(&info->size, len);
  return len;
}

ANN static ssize_t template_size(const Env env, struct tmpl_info* info) {
  ID_List base = info->cdef->base.tmpl->list;
  Type_List call = info->call;
  size_t size = tmpl_set(info, info->cdef->base.type);
  do {
    DECL_OB(const Type, t, = known_type(env, call->td))
    size += tmpl_set(info, t);
  } while((call = call->next) && (base = base->next) && ++size);
  return size + 16 + 3;
}

ANN static inline m_str tmpl_get(struct tmpl_info* info, m_str str) {
  const Type t = (Type)vector_at(&info->type, info->index);
  strcpy(str, t->name);
  return str += vector_at(&info->size, info->index);
}

ANN static void template_name(struct tmpl_info* info, m_str s) {
  m_str str = s;
  str = tmpl_get(info, str);
  *str++ = '<';
  *str++ = '~';
  const m_uint size = vector_size(&info->type);
  for(info->index = 1; info->index < size; ++info->index) {
    str = tmpl_get(info, str);
    if(info->index < size - 1)
      *str++ = ',';
    else {
      *str++ = '~';
      *str++ = '>';
    }
  }
  *str = '\0';
}

ANEW ANN static Symbol template_id(const Env env, const Class_Def c, const Type_List call) {
  struct tmpl_info info = { .cdef=c, .call=call };
  vector_init(&info.type);
  vector_init(&info.size);
  ssize_t sz = template_size(env, &info);
  char name[sz];
  if(sz > GW_ERROR)
    template_name(&info, name);
  vector_release(&info.type);
  vector_release(&info.size);
  return sz > GW_ERROR ? insert_symbol(env->gwion->st, name) : NULL;
}

ANN m_bool template_match(ID_List base, Type_List call) {
  while((call = call->next) && (base = base->next));
  return !call ? GW_OK : GW_ERROR;
}

ANN static Class_Def template_class(const Env env, const Class_Def def, const Type_List call) {
  DECL_OO(const Symbol, name, = template_id(env, def, call))
  if(env->class_def && name == insert_symbol(env->gwion->st, env->class_def->name))
     return env->class_def->e->def;
  const Type t = nspc_lookup_type1(env->curr, name);
  if(t)
    return t->e->def;
  const Class_Def c = cpy_class_def(env->gwion->mp, def);
  c->base.xid = name;
  SET_FLAG(c, template | ae_flag_ref);
  return c;

}

extern ANN m_bool scan0_class_def(const Env, const Class_Def);
extern ANN m_bool scan1_class_def(const Env, const Class_Def);
extern ANN m_bool traverse_func_def(const Env, const Func_Def);
extern ANN m_bool traverse_class_def(const Env, const Class_Def);

ANN static m_bool class2udef(const Env env, const Class_Def a, const Type t) {
  a->union_def = new_union_def(env->gwion->mp, a->list,
    loc_cpy(env->gwion->mp, t->e->def->pos));
  a->union_def->type_xid = a->base.xid;
  CHECK_BB(scan0_union_def(env, a->union_def))
  SET_FLAG(a, scan0);
  a->base.type = a->union_def->type;
  a->base.type->e->def = a;
  return GW_OK;
}

ANN static Type scan_class(const Env env, const Type t, const Type_Decl* td) {
  if(template_match(t->e->def->base.tmpl->list, td->types) < 0)
   ERR_O(td->xid->pos, _("invalid template types number"))
  DECL_OO(const Class_Def, a, = template_class(env, t->e->def, td->types))
  if(a->base.type)
    return a->base.type;
  a->base.tmpl = mk_tmpl(env, t->e->def->base.tmpl, td->types);
  if(t->e->parent !=  env->gwion->type[et_union])
    CHECK_BO(scan0_class_def(env, a))
  else
    CHECK_BO(class2udef(env, a, t))
  SET_FLAG(a->base.type, template);
  if(GET_FLAG(t, builtin))
    SET_FLAG(a->base.type, builtin);
  CHECK_BO(scan1_cdef(env, a))
  return a->base.type;
}

ANN static inline Symbol dot_symbol(SymTable *st, const Value v) {
  const m_str name = !GET_FLAG(v, static) ? "this" : v->from->owner_class->name;
  return insert_symbol(st, name);
}

ANN static inline Type dot_type(SymTable *st, const Value v) {
  const Type t = v->from->owner_class;
  if(!GET_FLAG(v, static))
    return t;
  const Value  val = nspc_lookup_value1(t->nspc->parent, insert_symbol(st, t->name));
  return val->type;
}

ANN Exp symbol_owned_exp(const Gwion gwion, const Symbol *data) {
  const Value v = prim_self(data)->value;
  const Exp base = new_prim_id(gwion->mp, dot_symbol(gwion->st, v), loc_cpy(gwion->mp, prim_pos(data)));
  const Exp dot = new_exp_dot(gwion->mp, base, *data);
  dot->d.exp_dot.t_base = dot->d.exp_dot.base->type = dot_type(gwion->st, v);
  dot->type = prim_exp(data)->type;
  dot->emit_var = prim_exp(data)->emit_var;
  return dot;
}

#include "sema.h"

#include <assert.h>
#include <stdlib.h>  // malloc

#include "expr.h"
#include "lexer.h"
#include "parser.h"
#include "type.h"
#include "util.h"
#include "var.h"

const int LF_BREAK = 1 << 0;
const int LF_CONTINUE = 1 << 0;

Defun *curfunc;
static int curloopflag;
static Node *curswitch;

// Scope

static Scope *enter_scope(Defun *defun, Vector *vars) {
  Scope *scope = new_scope(curscope, vars);
  curscope = scope;
  vec_push(defun->all_scopes, scope);
  return scope;
}

static void exit_scope(void) {
  assert(curscope != NULL);
  curscope = curscope->parent;
}

static VarInfo *add_cur_scope(const Token *ident, const Type *type, int flag) {
  if (curscope->vars == NULL)
    curscope->vars = new_vector();
  return var_add(curscope->vars, ident, type, flag);
}

static void fix_array_size(Type *type, Initializer *init) {
  assert(init != NULL);
  assert(type->type == TY_ARRAY);

  bool is_str = false;
  if (init->type != vMulti &&
      !(is_char_type(type->u.pa.ptrof) &&
        init->type == vSingle &&
        can_cast(type, init->u.single->valType, init->u.single, false) &&
        (is_str = true))) {
    parse_error(NULL, "Error initializer");
  }

  size_t arr_len = type->u.pa.length;
  if (arr_len == (size_t)-1) {
    if (is_str) {
      type->u.pa.length = init->u.single->u.str.size;
    } else {
      size_t index = 0;
      size_t max_index = 0;
      size_t i, len = init->u.multi->len;
      for (i = 0; i < len; ++i) {
        Initializer *init_elem = init->u.multi->data[i];
        if (init_elem->type == vArr) {
          assert(init_elem->u.arr.index->type == EX_NUM);
          index = init_elem->u.arr.index->u.num.ival;
        }
        ++index;
        if (max_index < index)
          max_index = index;
      }
      type->u.pa.length = max_index;
    }
  } else {
    assert(!is_str || init->u.single->type == EX_STR);
    size_t init_len = is_str ? init->u.single->u.str.size : (size_t)init->u.multi->len;
    if (init_len > arr_len)
      parse_error(NULL, "Initializer more than array size");
  }
}

static void add_func_label(const char *label) {
  assert(curfunc != NULL);
  if (curfunc->labels == NULL)
    curfunc->labels = new_map();
  map_put(curfunc->labels, label, label);  // Put dummy value.
}

static void add_func_goto(Node *node) {
  assert(curfunc != NULL);
  if (curfunc->gotos == NULL)
    curfunc->gotos = new_vector();
  vec_push(curfunc->gotos, node);
}

static Initializer *analyze_initializer(Initializer *init) {
  if (init == NULL)
    return NULL;

  switch (init->type) {
  case vSingle:
    init->u.single = analyze_expr(init->u.single, false);
    break;
  case vMulti:
    for (int i = 0; i < init->u.multi->len; ++i)
      init->u.multi->data[i] = analyze_initializer(init->u.multi->data[i]);
    break;
  case vDot:
    init->u.dot.value = analyze_initializer(init->u.dot.value);
    break;
  case vArr:
    init->u.arr.value = analyze_initializer(init->u.arr.value);
    break;
  }
  return init;
}

static Vector *clear_initial_value(Expr *expr, Vector *inits) {
  if (inits == NULL)
    inits = new_vector();

  switch (expr->valType->type) {
  case TY_NUM:
    {
      Num num = {0};
      Expr *zero = new_expr_numlit(expr->valType, NULL, &num);
      vec_push(inits,
               new_node_expr(new_expr_bop(EX_ASSIGN, expr->valType, NULL,
                                          expr, zero)));
    }
    break;
  case TY_PTR:
    {
      Num num = {0};
      Expr *zero = new_expr_numlit(expr->valType, NULL, &num);
      vec_push(inits,
               new_node_expr(new_expr_bop(EX_ASSIGN, expr->valType, NULL, expr,
                                          new_expr_cast(expr->valType, NULL, zero, true))));  // intptr_t
    }
    break;
  case TY_ARRAY:
    {
      size_t arr_len = expr->valType->u.pa.length;
      for (size_t i = 0; i < arr_len; ++i) {
        Num ofs = {.ival=i};
        Expr *add = add_expr(NULL, expr,
                             new_expr_numlit(&tyInt, NULL, &ofs), true);
        clear_initial_value(new_expr_deref(NULL, add), inits);
      }
    }
    break;
  case TY_STRUCT:
    {
      const StructInfo *sinfo = expr->valType->u.struct_.info;
      assert(!sinfo->is_union || !"Not implemented");
      assert(sinfo != NULL);
      for (int i = 0; i < sinfo->members->len; ++i) {
        VarInfo* varinfo = sinfo->members->data[i];
        Expr *member = new_expr_member(NULL, varinfo->type, expr, NULL, NULL, i);
        clear_initial_value(member, inits);
      }
    }
    break;
  default:
    assert(!"Not implemented");
    break;
  }

  return inits;
}

static void string_initializer(Expr *dst, Expr *src, Vector *inits) {
  // Initialize char[] with string literal (char s[] = "foo";).
  assert(dst->valType->type == TY_ARRAY && is_char_type(dst->valType->u.pa.ptrof));
  assert(src->valType->type == TY_ARRAY && is_char_type(src->valType->u.pa.ptrof));

  const char *str = src->u.str.buf;
  size_t size = src->u.str.size;
  size_t dstsize = dst->valType->u.pa.length;
  if (dstsize == (size_t)-1) {
    ((Type*)dst->valType)->u.pa.length = dstsize = size;
  } else {
    if (dstsize < size)
      parse_error(NULL, "Buffer is shorter than string: %d for \"%s\"", (int)dstsize, str);
  }

  for (size_t i = 0; i < size; ++i) {
    Num n = {.ival=i};
    Expr *index = new_expr_numlit(&tyInt, NULL, &n);
    vec_push(inits,
             new_node_expr(new_expr_bop(EX_ASSIGN, &tyChar, NULL,
                                        new_expr_deref(NULL, add_expr(NULL, dst, index, true)),
                                        new_expr_deref(NULL, add_expr(NULL, src, index, true)))));
  }
  if (dstsize > size) {
    Num z = {.ival=0};
    Expr *zero = new_expr_numlit(&tyChar, NULL, &z);
    for (size_t i = size; i < dstsize; ++i) {
      Num n = {.ival=i};
      Expr *index = new_expr_numlit(&tyInt, NULL, &n);
      vec_push(inits,
               new_node_expr(new_expr_bop(EX_ASSIGN, &tyChar, NULL,
                                          new_expr_deref(NULL, add_expr(NULL, dst, index, true)),
                                          zero)));
    }
  }
}

Initializer *flatten_initializer(const Type *type, Initializer *init) {
  if (init == NULL)
    return NULL;

  switch (type->type) {
  case TY_STRUCT:
    {
      if (init->type != vMulti)
        parse_error(NULL, "`{...}' expected for initializer");

      ensure_struct((Type*)type, NULL);
      const StructInfo *sinfo = type->u.struct_.info;
      int n = sinfo->members->len;
      int m = init->u.multi->len;
      if (n <= 0) {
        if (m > 0)
          parse_error(NULL, "Initializer for empty struct");
        return NULL;
      }
      if (sinfo->is_union && m > 1)
        parse_error(NULL, "Initializer for union more than 1");

      Initializer **values = malloc(sizeof(Initializer*) * n);
      for (int i = 0; i < n; ++i)
        values[i] = NULL;

      int index = 0;
      for (int i = 0; i < m; ++i) {
        Initializer *value = init->u.multi->data[i];
        if (value->type == vArr)
          parse_error(NULL, "indexed initializer for array");

        if (value->type == vDot) {
          index = var_find(sinfo->members, value->u.dot.name);
          if (index < 0)
            parse_error(NULL, "`%s' is not member of struct", value->u.dot.name);
          value = value->u.dot.value;
        }
        if (index >= n)
          parse_error(NULL, "Too many init values");

        // Allocate string literal for char* as a char array.
        if (value->type == vSingle && value->u.single->type == EX_STR) {
          const VarInfo *member = sinfo->members->data[index];
          if (member->type->type == TY_PTR &&
              is_char_type(member->type->u.pa.ptrof)) {
            Expr *expr = value->u.single;
            Initializer *strinit = malloc(sizeof(*strinit));
            strinit->type = vSingle;
            strinit->u.single = expr;

            // Create string and point to it.
            Type* strtype = arrayof(&tyChar, expr->u.str.size);
            const char * label = alloc_label();
            const Token *ident = alloc_ident(label, NULL, NULL);
            VarInfo *varinfo = define_global(strtype, VF_CONST | VF_STATIC, ident, NULL);
            varinfo->u.g.init = strinit;

            // Replace initializer from string literal to string array defined in global.
            value->u.single = new_expr_varref(label, strtype, true, ident);
          }
        }

        values[index++] = value;
      }

      Initializer *flat = malloc(sizeof(*flat));
      flat->type = vMulti;
      //flat->u.multi = new_vector();
      Vector *v = malloc(sizeof(*v));
      v->len = v->capacity = n;
      v->data = (void**)values;
      flat->u.multi = v;

      return flat;
    }
  case TY_ARRAY:
    switch (init->type) {
    case vMulti:
      if (init->type != vMulti)
        parse_error(NULL, "`{...}' expected for initializer");
      // Check whether vDot exists.
      for (int i = 0, len = init->u.multi->len; i < len; ++i) {
        Initializer *init_elem = init->u.multi->data[i];
        if (init_elem->type == vDot)
          parse_error(NULL, "dot initializer for array");
      }
      break;
    case vSingle:
      // Special handling for string (char[]).
      if (can_cast(type, init->u.single->valType, init->u.single, false))
        break;
      // Fallthrough
    default:
      parse_error(NULL, "Illegal initializer");
      break;
    }
  default:
    break;
  }
  return init;
}

static Initializer *check_global_initializer(const Type *type, Initializer *init) {
  if (init == NULL)
    return NULL;

  init = flatten_initializer(type, init);

  switch (type->type) {
  case TY_NUM:
    if (init->type == vSingle) {
      switch (init->u.single->type) {
      case EX_NUM:
        return init;
      default:
        parse_error(NULL, "initializer type error");
        break;
      }
    }
    break;
  case TY_PTR:
    {
      if (init->type != vSingle)
        parse_error(NULL, "initializer type error");
      Expr *value = init->u.single;
      switch (value->type) {
      case EX_REF:
        {
          value = value->u.unary.sub;
          if (value->type != EX_VARREF)
            parse_error(NULL, "pointer initializer must be varref");
          if (!value->u.varref.global)
            parse_error(NULL, "Allowed global reference only");

          VarInfo *info = find_global(value->u.varref.ident);
          assert(info != NULL);

          if (!same_type(type->u.pa.ptrof, info->type))
            parse_error(NULL, "Illegal type");

          return init;
        }
      case EX_VARREF:
        {
          if (!value->u.varref.global)
            parse_error(NULL, "Allowed global reference only");

          VarInfo *info = find_global(value->u.varref.ident);
          assert(info != NULL);

          if (info->type->type != TY_ARRAY || !same_type(type->u.pa.ptrof, info->type->u.pa.ptrof))
            parse_error(NULL, "Illegal type");

          return init;
        }
      case EX_CAST:
        {  // Handle NULL assignment.
          while (value->type == EX_CAST)
            value = value->u.unary.sub;
          if (is_number(value->valType->type)) {
            Initializer *init2 = malloc(sizeof(*init2));
            init2->type = vSingle;
            init2->u.single = value;
            return init2;
          }
        }
        break;
      case EX_STR:
        {
          if (!(is_char_type(type->u.pa.ptrof) && value->type == EX_STR))
            parse_error(NULL, "Illegal type");

          // Create string and point to it.
          Type* type2 = arrayof(type->u.pa.ptrof, value->u.str.size);
          const char *label = alloc_label();
          const Token *ident = alloc_ident(label, NULL, NULL);
          VarInfo *varinfo = define_global(type2, VF_CONST | VF_STATIC, ident, NULL);
          varinfo->u.g.init = init;

          Initializer *init2 = malloc(sizeof(*init2));
          init2->type = vSingle;
          init2->u.single = new_expr_varref(label, type2, true, ident);
          return init2;
        }
      default:
        break;
      }
      parse_error(NULL, "initializer type error: type=%d", value->type);
    }
    break;
  case TY_ARRAY:
    switch (init->type) {
    case vMulti:
      {
        const Type *elemtype = type->u.pa.ptrof;
        Vector *multi = init->u.multi;
        for (int i = 0, len = multi->len; i < len; ++i) {
          Initializer *eleminit = multi->data[i];
          multi->data[i] = check_global_initializer(elemtype, eleminit);
        }
      }
      break;
    case vSingle:
      if (is_char_type(type->u.pa.ptrof) && init->u.single->type == EX_STR) {
        assert(type->u.pa.length != (size_t)-1);
        if (type->u.pa.length < init->u.single->u.str.size) {
          parse_error(NULL, "Array size shorter than initializer");
        }
        return init;
      }
      // Fallthrough
    case vDot:
    default:
      parse_error(NULL, "Illegal initializer");
      break;
    }
    break;
  case TY_STRUCT:
    {
      const StructInfo *sinfo = type->u.struct_.info;
      for (int i = 0, n = sinfo->members->len; i < n; ++i) {
        VarInfo* varinfo = sinfo->members->data[i];
        Initializer *init_elem = init->u.multi->data[i];
        if (init_elem != NULL)
          init->u.multi->data[i] = check_global_initializer(varinfo->type, init_elem);
      }
    }
    break;
  default:
    parse_error(NULL, "Global initial value for type %d not implemented (yet)\n", type->type);
    break;
  }
  return init;
}

static Vector *assign_initial_value(Expr *expr, Initializer *init, Vector *inits) {
  if (inits == NULL)
    inits = new_vector();

  Initializer *org_init = init;
  init = flatten_initializer(expr->valType, init);

  switch (expr->valType->type) {
  case TY_ARRAY:
    switch (init->type) {
    case vMulti:
      {
        size_t arr_len = expr->valType->u.pa.length;
        assert(arr_len != (size_t)-1);
        if ((size_t)init->u.multi->len > arr_len)
          parse_error(NULL, "Initializer more than array size");
        size_t len = init->u.multi->len;
        size_t index = 0;
        for (size_t i = 0; i < len; ++i, ++index) {
          Initializer *init_elem = init->u.multi->data[i];
          if (init_elem->type == vArr) {
            Expr *ind = init_elem->u.arr.index;
            if (ind->type != EX_NUM)
              parse_error(NULL, "Number required");
            index = ind->u.num.ival;
            init_elem = init_elem->u.arr.value;
          }

          Num n = {.ival=index};
          Expr *add = add_expr(NULL, expr, new_expr_numlit(&tyInt, NULL, &n), true);

          assign_initial_value(new_expr_deref(NULL, add), init_elem, inits);
        }
      }
      break;
    case vSingle:
      // Special handling for string (char[]).
      if (can_cast(expr->valType, init->u.single->valType, init->u.single, false)) {
        string_initializer(expr, init->u.single, inits);
        break;
      }
      // Fallthrough
    default:
      parse_error(NULL, "Error initializer");
      break;

    }
    break;
  case TY_STRUCT:
    {
      if (init->type != vMulti)
        parse_error(NULL, "`{...}' expected for initializer");

      const StructInfo *sinfo = expr->valType->u.struct_.info;
      if (!sinfo->is_union) {
        for (int i = 0, n = sinfo->members->len; i < n; ++i) {
          VarInfo* varinfo = sinfo->members->data[i];
          Expr *member = new_expr_member(NULL, varinfo->type, expr, NULL, NULL, i);
          Initializer *init_elem = init->u.multi->data[i];
          if (init_elem != NULL)
            assign_initial_value(member, init_elem, inits);
          else
            clear_initial_value(member, inits);
        }
      } else {
        int n = sinfo->members->len;
        int m = init->u.multi->len;
        if (n <= 0 && m > 0)
          parse_error(NULL, "Initializer for empty union");
        if (org_init->u.multi->len > 1)
          parse_error(NULL, "More than one initializer for union");

        for (int i = 0; i < n; ++i) {
          Initializer *init_elem = init->u.multi->data[i];
          if (init_elem == NULL)
            continue;
          VarInfo* varinfo = sinfo->members->data[i];
          Expr *member = new_expr_member(NULL, varinfo->type, expr, NULL, NULL, i);
          assign_initial_value(member, init_elem, inits);
          break;
        }
      }
    }
    break;
  default:
    if (init->type != vSingle)
      parse_error(NULL, "Error initializer");
    vec_push(inits,
             new_node_expr(new_expr_bop(EX_ASSIGN, expr->valType, NULL, expr,
                                        new_expr_cast(expr->valType, NULL, init->u.single, false))));
    break;
  }

  return inits;
}

static Node *sema_vardecl(Node *node) {
  assert(node->type == ND_VARDECL);
  Vector *decls = node->u.vardecl.decls;
  Vector *inits = NULL;
  for (int i = 0, len = decls->len; i < len; ++i) {
    VarDecl *decl = decls->data[i];
    const Type *type = decl->type;
    const Token *ident = decl->ident;
    int flag = decl->flag;
    Initializer *init = decl->init;

    if (type->type == TY_ARRAY && init != NULL)
      fix_array_size((Type*)type, init);

    if (curfunc != NULL) {
      VarInfo *varinfo = add_cur_scope(ident, type, flag);
      init = analyze_initializer(init);

      // TODO: Check `init` can be cast to `type`.
      if (flag & VF_STATIC) {
        varinfo->u.g.init = check_global_initializer(type, init);
        // static variable initializer is handled in codegen, same as global variable.
      } else if (init != NULL) {
        inits = assign_initial_value(
            new_expr_varref(ident->u.ident, type, false, NULL), init, inits);
      }
    } else {
      if (flag & VF_EXTERN && init != NULL)
        parse_error(/*tok*/ NULL, "extern with initializer");
      // Toplevel
      VarInfo *varinfo = define_global(type, flag, ident, NULL);
      assert(varinfo != NULL);
      init = analyze_initializer(init);
      varinfo->u.g.init = check_global_initializer(type, init);
    }
  }

  node->u.vardecl.inits = inits;
  return node;
}

static void sema_nodes(Vector *nodes) {
  if (nodes == NULL)
    return;
  for (int i = 0, len = nodes->len; i < len; ++i)
    nodes->data[i] = sema(nodes->data[i]);
}

static void sema_defun(Defun *defun) {
  const Token *ident = NULL;

  Vector *param_types = NULL;
  if (defun->params != NULL) {
    param_types = new_vector();
    for (int i = 0, len = defun->params->len; i < len; ++i)
      vec_push(param_types, ((VarInfo*)defun->params->data[i])->type);
  }
  defun->type = new_func_type(defun->rettype, param_types, defun->vaargs);

  VarInfo *def = find_global(defun->name);
  if (def == NULL) {
    define_global(defun->type, defun->flag | VF_CONST, ident, defun->name);
  } else {
    if (def->type->type != TY_FUNC)
      parse_error(ident, "Definition conflict: `%s'");
    // TODO: Check type.
    // TODO: Check duplicated definition.
    if (def->u.g.init != NULL)
      parse_error(ident, "`%s' function already defined");
  }

  if (defun->stmts != NULL) {  // Not prototype defintion.
    curfunc = defun;
    enter_scope(defun, defun->params);  // Scope for parameters.
    curscope = defun->top_scope = enter_scope(defun, NULL);
    sema_nodes(defun->stmts);
    exit_scope();
    exit_scope();
    curfunc = NULL;
    curscope = NULL;

    // Check goto labels.
    if (defun->gotos != NULL) {
      Vector *gotos = defun->gotos;
      Map *labels = defun->labels;
      for (int i = 0; i < gotos->len; ++i) {
        Node *node = gotos->data[i];
        if (labels == NULL || map_get(labels, node->u.goto_.ident) == NULL)
          parse_error(node->u.goto_.tok, "`%s' not found", node->u.goto_.ident);
      }
    }
  }
}

Node *sema(Node *node) {
  if (node == NULL)
    return node;

  switch (node->type) {
  case ND_EXPR:
    node->u.expr = analyze_expr(node->u.expr, false);
    break;

  case ND_DEFUN:
    sema_defun(node->u.defun);
    break;

  case ND_BLOCK:
    {
      Scope *parent_scope = curscope;
      if (curfunc != NULL)
        node->u.block.scope = curscope = enter_scope(curfunc, NULL);
      sema_nodes(node->u.block.nodes);
      curscope = parent_scope;
    }
    break;

  case ND_IF:
    node->u.if_.cond = analyze_expr(node->u.if_.cond, false);
    node->u.if_.tblock = sema(node->u.if_.tblock);
    node->u.if_.fblock = sema(node->u.if_.fblock);
    break;

  case ND_SWITCH:
    {
      Node *save_switch = curswitch;
      int save_flag = curloopflag;
      curloopflag |= LF_BREAK;
      curswitch = node;

      node->u.switch_.value = analyze_expr(node->u.switch_.value, false);
      node->u.switch_.body = sema(node->u.switch_.body);

      curloopflag = save_flag;
      curswitch = save_switch;
    }
    break;

  case ND_WHILE:
  case ND_DO_WHILE:
    {
      node->u.while_.cond = analyze_expr(node->u.while_.cond, false);

      int save_flag = curloopflag;
      curloopflag |= LF_BREAK | LF_CONTINUE;

      node->u.while_.body = sema(node->u.while_.body);

      curloopflag = save_flag;
    }
    break;

  case ND_FOR:
    {
      node->u.for_.pre = analyze_expr(node->u.for_.pre, false);
      node->u.for_.cond = analyze_expr(node->u.for_.cond, false);
      node->u.for_.post = analyze_expr(node->u.for_.post, false);

      int save_flag = curloopflag;
      curloopflag |= LF_BREAK | LF_CONTINUE;

      node->u.for_.body = sema(node->u.for_.body);

      curloopflag = save_flag;
    }
    break;

  case ND_BREAK:
    if ((curloopflag & LF_BREAK) == 0)
      parse_error(/*tok*/ NULL, "`break' cannot be used outside of loop");
    break;

  case ND_CONTINUE:
    if ((curloopflag & LF_CONTINUE) == 0)
      parse_error(/*tok*/ NULL, "`continue' cannot be used outside of loop");
    break;

  case ND_RETURN:
    {
      assert(curfunc != NULL);
      const Type *rettype = curfunc->type->u.func.ret;
      Expr *val = node->u.return_.val;
      Token *tok = NULL;
      if (val == NULL) {
        if (rettype->type != TY_VOID)
          parse_error(tok, "`return' required a value");
      } else {
        if (rettype->type == TY_VOID)
          parse_error(tok, "void function `return' a value");

        const Token *tok = NULL;
        Expr *val = analyze_expr(node->u.return_.val, false);
        node->u.return_.val = new_expr_cast(rettype, tok, val, false);
      }
    }
    break;

  case ND_CASE:
    {
      if (curswitch == NULL)
        parse_error(/*tok*/ NULL, "`case' cannot use outside of `switch`");

      intptr_t value = node->u.case_.value;
      // Check duplication.
      Vector *values = curswitch->u.switch_.case_values;
      for (int i = 0, len = values->len; i < len; ++i) {
        if ((intptr_t)values->data[i] == value)
          parse_error(/*tok*/ NULL, "Case value `%lld' already defined: %s", value);
      }
      vec_push(values, (void*)value);
    }
    break;

  case ND_DEFAULT:
    if (curswitch == NULL)
      parse_error(/*tok*/ NULL, "`default' cannot use outside of `switch'");
    if (curswitch->u.switch_.has_default)
      parse_error(/*tok*/ NULL, "`default' already defined in `switch'");

    curswitch->u.switch_.has_default = true;
    break;

  case ND_GOTO:
    add_func_goto(node);
    break;

  case ND_LABEL:
    add_func_label(node->u.label.name);
    node->u.label.stmt = sema(node->u.label.stmt);
    break;

  case ND_VARDECL:
    return sema_vardecl(node);

  case ND_TOPLEVEL:
    sema_nodes(node->u.toplevel.nodes);
    break;

  default:
    fprintf(stderr, "sema: Unhandled node, type=%d\n", node->type);
    assert(false);
    break;
  }
  return node;
}

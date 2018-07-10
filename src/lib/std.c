#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "type.h"
#include "instr.h"
#include "import.h"
#include <dirent.h>
#ifdef USE_DOUBLE
#define fabs fabs
#undef USE_DOUBLE
#else
#define fabs fabsf
#endif
#include <soundpipe.h> // for sp_rand
#include <err_msg.h>

#define LOGTWO log(2)
#define LOGTEN log(10)

static double mtof(const double f) {
  if(f <= -1500)
    return 0.0;
  if(f > 1499)
    return mtof(1499);
// else return (8.17579891564 * exp(.0577622650 * f));
  return (pow(2, (f - 69) / 12.0) * 440.0);
}

static double ftom(const double f) {
  // return (f > 0 ? 17.3123405046 * log(.12231220585 * f) : -1500);
  return (f > 0 ? (log(f / 440.0) / LOGTWO) * 12.0 + 69 : -1500);
}
static double powtodb(const double f) {
  if(f <= 0)
    return (0);
  const double val = 100 + 10. / LOGTEN * log(f);
  return (val < 0 ? 0 : val);
}

static double rmstodb(const double f) {
  if(f <= 0)
    return (0);
  const double val = 100 + 20. / LOGTEN * log(f);
  return (val < 0 ? 0 : val);
}

static double dbtopow(double f) {
  if(f <= 0)
    return (0);
  if(f > 870) f = 870;
    return (exp((LOGTEN * 0.1) * (f - 100.)));
}
static double dbtorms(double f) {
  if(f <= 0)
    return (0);
  if(f > 485) f = 485;
    return (exp((LOGTEN * 0.05) * (f - 100.)));
}

#define GETSTRING(a, b)                          \
  const M_Object a##_obj = *(M_Object*)MEM((b)); \
  if(!a##_obj)                                   \
    Except(shred, "NullPtrException");           \
  const m_str a = STRING(a##_obj);               \
  release(a##_obj, shred);

static SFUN(std_system) {
  GETSTRING(str, SZ_INT);
  *(m_uint*)RETURN = system(str);
}

#define STDGET                                \
  const m_int ret = *(m_int*)MEM(SZ_INT);     \
  const m_int min = *(m_int*)MEM(SZ_INT * 2); \
  const m_int max = *(m_int*)MEM(SZ_INT * 3); \

static SFUN(std_clamp) {
  STDGET
  *(m_uint*)RETURN = ret < min ? min : ret > max ? max : ret;
}

static SFUN(std_clampf) {
  STDGET
  *(m_float*)RETURN = ret < min ? min : ret > max ? max : ret;
}

static SFUN(std_scale) {
  STDGET
  const m_float min2 = *(m_float*)MEM(SZ_INT + SZ_FLOAT * 3);
  const m_float max2 = *(m_float*)MEM(SZ_INT + SZ_FLOAT * 4);
  *(m_float*)RETURN = min2 + (max2 - min2) * ((ret - min) / (max - min));
}

static SFUN(std_getenv) {
  GETSTRING(env, SZ_INT)
  const m_str str = getenv(env);
  *(M_Object*)RETURN = str ? new_String(shred, str) : 0;
}

static SFUN(std_setenv) {
  GETSTRING(key, SZ_INT)
  GETSTRING(val, SZ_INT * 2)
  *(m_uint*)RETURN = setenv(key, val, 1);
}

static SFUN(std_atoi) {
  GETSTRING(val, SZ_INT)
  *(m_uint*)RETURN = strtol(val, NULL, 10);
}

static SFUN(std_atof) {
  GETSTRING(val, SZ_INT)
  *(m_float*)RETURN = atof(val);
}

static SFUN(std_itoa) {
  char c[1024];
  const m_int value = *(m_int*)MEM(SZ_INT);
  sprintf(c, "%"INT_F, value);
  *(M_Object*)RETURN = new_String(shred, c);
}

static SFUN(std_ftoa) {
  char c[1024];
  const m_float value = *(m_float*)MEM(SZ_INT);
  sprintf(c, "%f", value);
  *(M_Object*)RETURN = new_String(shred, c);
}

#define pow10(a) pow(10.0, (a) / 20.0)
#define std(name, func)\
static SFUN(std_##name) {\
  *(m_float*)RETURN = func(*(m_float*)MEM(SZ_INT));\
}
std(mtof, mtof)
std(ftom, ftom)
std(powtodb, powtodb)
std(rmstodb, rmstodb)
std(dbtopow, dbtopow)
std(dbtorms, dbtorms)
std(dbtolin, pow10)
std(lintodb, 20.0 * log10)

ANN m_bool import_std(const Gwi gwi) {
  const Type t_std = gwi_mk_type(gwi, "Std", 0, NULL);
  CHECK_OB(t_std)
  CHECK_BB(gwi_class_ini(gwi,  t_std, NULL, NULL))

  gwi_func_ini(gwi, "int", "clamp", std_clamp);
  gwi_func_arg(gwi, "int", "value");
  gwi_func_arg(gwi, "int", "min");
  gwi_func_arg(gwi, "int", "max");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "float", "clampf", std_clampf);
  gwi_func_arg(gwi, "float", "value");
  gwi_func_arg(gwi, "float", "min");
  gwi_func_arg(gwi, "float", "max");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "float", "scale", std_scale);
  gwi_func_arg(gwi, "float", "value");
  gwi_func_arg(gwi, "float", "srcmin");
  gwi_func_arg(gwi, "float", "srcmax");
  gwi_func_arg(gwi, "float", "dstmin");
  gwi_func_arg(gwi, "float", "dstmax");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "int", "system", std_system);
  gwi_func_arg(gwi, "string", "cmd");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "string", "getenv", std_getenv);
  gwi_func_arg(gwi, "string", "key");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "int", "setenv", std_setenv);
  gwi_func_arg(gwi, "string", "key");
  gwi_func_arg(gwi, "string", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "int", "atoi", std_atoi);
  gwi_func_arg(gwi, "string", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "float", "atof", std_atof);
  gwi_func_arg(gwi, "string", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "string", "itoa", std_itoa);
  gwi_func_arg(gwi, "float", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "string", "ftoa", std_ftoa);
  gwi_func_arg(gwi, "float", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "float", "mtof", std_mtof);
  gwi_func_arg(gwi, "float", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "float", "ftom", std_ftom);
  gwi_func_arg(gwi, "float", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "float", "powtodb", std_powtodb);
  gwi_func_arg(gwi, "float", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "float", "rmstodb", std_rmstodb);
  gwi_func_arg(gwi, "float", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "float", "dbtopow", std_dbtopow);
  gwi_func_arg(gwi, "float", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "float", "dbtorms", std_dbtorms);
  gwi_func_arg(gwi, "float", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "float", "dbtolin", std_dbtolin);
  gwi_func_arg(gwi, "float", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))

  gwi_func_ini(gwi, "float", "lintodb", std_lintodb);
  gwi_func_arg(gwi, "float", "value");
  CHECK_BB(gwi_func_end(gwi, ae_flag_static))
  CHECK_BB(gwi_class_end(gwi))
  return 1;
}

#ifdef JIT
#include "ctrl/std.h"
#endif

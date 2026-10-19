/* Color LCD: see lcd.h. Every primitive is a literal port of engine.js colorLcd() (itself after the
 * firmware's BitmapBuffer): the game's layout and its tests were tuned against these exact pixels.
 *
 * Lua hands over int32 values and JS computes with doubles, so whatever can leave the int32 range
 * (x + w, b - a + 1, r * r) runs in int64 or double here. Products that JS rounds to a double
 * before adding go through fmul(), so a compiler that fuses multiply-add (aarch64, x86 with FMA)
 * cannot change a result. Rows and points that the JS loops visit but cannot draw (off the
 * screen) are skipped arithmetically: a triangle thousands of rows tall costs its visible rows. */
#include "lcd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RGB565C(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define NAMED_LIST(X) \
  X(COLOR_THEME_PRIMARY1, 0, 0, 0) X(COLOR_THEME_PRIMARY2, 255, 255, 255) X(COLOR_THEME_PRIMARY3, 12, 63, 102) \
  X(COLOR_THEME_SECONDARY1, 18, 94, 153) X(COLOR_THEME_SECONDARY2, 182, 224, 255) \
  X(COLOR_THEME_SECONDARY3, 228, 238, 242) X(COLOR_THEME_FOCUS, 20, 161, 229) X(COLOR_THEME_EDIT, 0, 153, 9) \
  X(COLOR_THEME_ACTIVE, 255, 222, 0) X(COLOR_THEME_WARNING, 224, 0, 0) X(COLOR_THEME_DISABLED, 140, 140, 140) \
  X(BLACK, 0, 0, 0) X(WHITE, 255, 255, 255) X(LIGHTWHITE, 238, 238, 238) X(LIGHTGREY, 192, 192, 192) \
  X(GREY, 150, 150, 150) X(DARKGREY, 64, 64, 64) X(RED, 229, 32, 30) X(DARKRED, 160, 0, 6) \
  X(LIGHTRED, 255, 128, 128) X(GREEN, 25, 150, 50) X(DARKGREEN, 0, 120, 0) X(BRIGHTGREEN, 0, 180, 60) \
  X(BLUE, 0x30, 0xA0, 0xE0) X(DARKBLUE, 0, 0, 140) X(CYAN, 0, 210, 255) X(YELLOW, 0xF0, 0xD0, 0x10) \
  X(LIGHTBROWN, 156, 109, 32) X(DARKBROWN, 106, 72, 16) X(ORANGE, 229, 100, 30) X(MAGENTA, 255, 0, 255)

#define X_NAMED(n, r, g, b) { #n, r, g, b },
#define X_565(n, r, g, b) RGB565C(r, g, b),
const lcd_named_t LCD_NAMED[LCD_NAMED_COLORS] = { NAMED_LIST(X_NAMED) };
static const uint16_t NAMED565[LCD_NAMED_COLORS] = { NAMED_LIST(X_565) };

/* Math.cos / Math.sin (V8's fdlibm) of a * Math.PI / 180, the 360 points of drawCircle, so they
 * don't depend on the last bit of the C library's cos (generated with node) */
static const double COS[360] = {
  1.0, 0.9998476951563913, 0.9993908270190958, 0.9986295347545738,
  0.9975640502598242, 0.9961946980917455, 0.9945218953682733, 0.992546151641322,
  0.9902680687415704, 0.9876883405951378, 0.984807753012208, 0.981627183447664,
  0.9781476007338057, 0.9743700647852352, 0.9702957262759965, 0.9659258262890683,
  0.9612616959383189, 0.9563047559630354, 0.9510565162951535, 0.9455185755993168,
  0.9396926207859084, 0.9335804264972017, 0.9271838545667874, 0.9205048534524404,
  0.9135454576426009, 0.9063077870366499, 0.898794046299167, 0.8910065241883679,
  0.882947592858927, 0.8746197071393957, 0.8660254037844387, 0.8571673007021123,
  0.848048096156426, 0.838670567945424, 0.8290375725550416, 0.8191520442889918,
  0.8090169943749475, 0.7986355100472928, 0.788010753606722, 0.7771459614569709,
  0.766044443118978, 0.754709580222772, 0.7431448254773942, 0.7313537016191706,
  0.7193398003386512, 0.7071067811865476, 0.6946583704589974, 0.6819983600624985,
  0.6691306063588582, 0.6560590289905073, 0.6427876096865394, 0.6293203910498375,
  0.6156614753256583, 0.6018150231520483, 0.5877852522924731, 0.573576436351046,
  0.5591929034707468, 0.5446390350150272, 0.5299192642332049, 0.5150380749100544,
  0.5000000000000001, 0.4848096202463371, 0.46947156278589086, 0.45399049973954686,
  0.43837114678907746, 0.42261826174069944, 0.4067366430758002, 0.39073112848927394,
  0.37460659341591196, 0.3583679495453004, 0.3420201433256688, 0.32556815445715676,
  0.30901699437494745, 0.29237170472273677, 0.27563735581699916, 0.25881904510252074,
  0.2419218955996679, 0.22495105434386492, 0.20791169081775945, 0.19080899537654492,
  0.17364817766693041, 0.15643446504023092, 0.1391731009600657, 0.12186934340514749,
  0.10452846326765346, 0.08715574274765814, 0.06975647374412546, 0.052335956242943966,
  0.03489949670250108, 0.017452406437283376, 6.123233995736766e-17, -0.017452406437283477,
  -0.03489949670250073, -0.05233595624294362, -0.06975647374412533, -0.08715574274765824,
  -0.10452846326765333, -0.12186934340514737, -0.13917310096006535, -0.15643446504023104,
  -0.1736481776669303, -0.1908089953765448, -0.20791169081775912, -0.2249510543438648,
  -0.24192189559966779, -0.25881904510252085, -0.27563735581699905, -0.29237170472273666,
  -0.30901699437494734, -0.3255681544571564, -0.3420201433256687, -0.35836794954530027,
  -0.37460659341591207, -0.3907311284892736, -0.4067366430758001, -0.42261826174069933,
  -0.4383711467890775, -0.45399049973954675, -0.46947156278589053, -0.484809620246337,
  -0.4999999999999998, -0.5150380749100543, -0.5299192642332048, -0.5446390350150271,
  -0.5591929034707467, -0.5735764363510458, -0.587785252292473, -0.6018150231520484,
  -0.6156614753256583, -0.6293203910498373, -0.6427876096865394, -0.6560590289905075,
  -0.6691306063588582, -0.6819983600624984, -0.694658370458997, -0.7071067811865475,
  -0.7193398003386513, -0.7313537016191705, -0.7431448254773941, -0.754709580222772,
  -0.7660444431189779, -0.7771459614569707, -0.7880107536067219, -0.7986355100472929,
  -0.8090169943749473, -0.8191520442889916, -0.8290375725550416, -0.8386705679454242,
  -0.848048096156426, -0.8571673007021122, -0.8660254037844387, -0.8746197071393957,
  -0.8829475928589268, -0.8910065241883678, -0.898794046299167, -0.9063077870366499,
  -0.9135454576426008, -0.9205048534524402, -0.9271838545667873, -0.9335804264972017,
  -0.9396926207859083, -0.9455185755993167, -0.9510565162951535, -0.9563047559630354,
  -0.9612616959383187, -0.9659258262890682, -0.9702957262759965, -0.9743700647852352,
  -0.9781476007338057, -0.981627183447664, -0.984807753012208, -0.9876883405951377,
  -0.9902680687415703, -0.992546151641322, -0.9945218953682733, -0.9961946980917455,
  -0.9975640502598242, -0.9986295347545738, -0.9993908270190958, -0.9998476951563913,
  -1.0, -0.9998476951563913, -0.9993908270190958, -0.9986295347545738,
  -0.9975640502598243, -0.9961946980917455, -0.9945218953682734, -0.992546151641322,
  -0.9902680687415703, -0.9876883405951378, -0.984807753012208, -0.981627183447664,
  -0.9781476007338057, -0.9743700647852352, -0.9702957262759966, -0.9659258262890684,
  -0.9612616959383189, -0.9563047559630355, -0.9510565162951535, -0.9455185755993167,
  -0.9396926207859084, -0.9335804264972017, -0.9271838545667874, -0.9205048534524404,
  -0.9135454576426011, -0.90630778703665, -0.8987940462991671, -0.8910065241883681,
  -0.8829475928589269, -0.8746197071393959, -0.8660254037844386, -0.8571673007021123,
  -0.8480480961564261, -0.838670567945424, -0.8290375725550418, -0.819152044288992,
  -0.8090169943749475, -0.798635510047293, -0.7880107536067222, -0.7771459614569708,
  -0.7660444431189781, -0.7547095802227719, -0.7431448254773942, -0.7313537016191706,
  -0.7193398003386511, -0.7071067811865477, -0.6946583704589975, -0.6819983600624989,
  -0.6691306063588585, -0.6560590289905077, -0.6427876096865395, -0.6293203910498371,
  -0.6156614753256581, -0.6018150231520483, -0.5877852522924732, -0.5735764363510464,
  -0.5591929034707472, -0.544639035015027, -0.529919264233205, -0.5150380749100545,
  -0.5000000000000004, -0.48480962024633684, -0.46947156278589075, -0.4539904997395469,
  -0.43837114678907774, -0.42261826174069994, -0.4067366430758001, -0.3907311284892738,
  -0.3746065934159123, -0.3583679495453007, -0.3420201433256694, -0.32556815445715664,
  -0.30901699437494756, -0.2923717047227371, -0.2756373558169989, -0.25881904510252063,
  -0.24192189559966779, -0.22495105434386525, -0.2079116908177598, -0.19080899537654547,
  -0.17364817766693033, -0.15643446504023104, -0.13917310096006494, -0.12186934340514717,
  -0.10452846326765336, -0.08715574274765825, -0.06975647374412558, -0.052335956242944306,
  -0.03489949670250165, -0.017452406437283498, -1.8369701987210297e-16, 0.01745240643728313,
  0.03489949670250128, 0.052335956242943946, 0.06975647374412522, 0.08715574274765789,
  0.10452846326765299, 0.12186934340514768, 0.13917310096006547, 0.15643446504023067,
  0.17364817766692997, 0.19080899537654425, 0.20791169081775857, 0.22495105434386492,
  0.24192189559966745, 0.25881904510252113, 0.2756373558169994, 0.2923717047227367,
  0.30901699437494723, 0.3255681544571563, 0.34202014332566816, 0.35836794954529955,
  0.37460659341591196, 0.3907311284892735, 0.40673664307580054, 0.4226182617406996,
  0.4383711467890774, 0.45399049973954664, 0.4694715627858904, 0.4848096202463365,
  0.5000000000000001, 0.5150380749100542, 0.5299192642332047, 0.5446390350150266,
  0.5591929034707462, 0.573576436351046, 0.5877852522924729, 0.6018150231520479,
  0.6156614753256585, 0.6293203910498375, 0.6427876096865393, 0.656059028990507,
  0.6691306063588578, 0.6819983600624979, 0.6946583704589966, 0.7071067811865474,
  0.7193398003386509, 0.7313537016191707, 0.7431448254773942, 0.7547095802227719,
  0.7660444431189778, 0.7771459614569706, 0.7880107536067216, 0.7986355100472928,
  0.8090169943749473, 0.8191520442889916, 0.8290375725550414, 0.838670567945424,
  0.8480480961564254, 0.8571673007021121, 0.8660254037844384, 0.8746197071393959,
  0.882947592858927, 0.8910065241883678, 0.8987940462991671, 0.9063077870366497,
  0.913545457642601, 0.9205048534524399, 0.9271838545667873, 0.9335804264972015,
  0.9396926207859084, 0.9455185755993165, 0.9510565162951535, 0.9563047559630357,
  0.9612616959383187, 0.9659258262890683, 0.9702957262759965, 0.9743700647852351,
  0.9781476007338056, 0.981627183447664, 0.9848077530122079, 0.9876883405951377,
  0.9902680687415703, 0.992546151641322, 0.9945218953682733, 0.9961946980917455,
  0.9975640502598243, 0.9986295347545738, 0.9993908270190958, 0.9998476951563913,
};
static const double SIN[360] = {
  0.0, 0.01745240643728351, 0.03489949670250097, 0.05233595624294383,
  0.0697564737441253, 0.08715574274765817, 0.10452846326765346, 0.12186934340514748,
  0.13917310096006544, 0.15643446504023087, 0.17364817766693033, 0.1908089953765448,
  0.20791169081775931, 0.224951054343865, 0.24192189559966773, 0.25881904510252074,
  0.27563735581699916, 0.29237170472273677, 0.3090169943749474, 0.32556815445715664,
  0.3420201433256687, 0.35836794954530027, 0.374606593415912, 0.3907311284892737,
  0.40673664307580015, 0.42261826174069944, 0.4383711467890774, 0.45399049973954675,
  0.4694715627858908, 0.48480962024633706, 0.49999999999999994, 0.5150380749100542,
  0.5299192642332049, 0.5446390350150271, 0.5591929034707469, 0.573576436351046,
  0.5877852522924731, 0.6018150231520483, 0.6156614753256582, 0.6293203910498374,
  0.6427876096865393, 0.6560590289905072, 0.6691306063588582, 0.6819983600624985,
  0.6946583704589973, 0.7071067811865475, 0.7193398003386511, 0.7313537016191705,
  0.7431448254773942, 0.754709580222772, 0.766044443118978, 0.7771459614569708,
  0.788010753606722, 0.7986355100472928, 0.8090169943749475, 0.8191520442889918,
  0.8290375725550417, 0.8386705679454239, 0.8480480961564261, 0.8571673007021122,
  0.8660254037844386, 0.8746197071393957, 0.8829475928589269, 0.8910065241883678,
  0.898794046299167, 0.9063077870366499, 0.9135454576426009, 0.9205048534524403,
  0.9271838545667874, 0.9335804264972017, 0.9396926207859083, 0.9455185755993167,
  0.9510565162951535, 0.9563047559630354, 0.9612616959383189, 0.9659258262890683,
  0.9702957262759965, 0.9743700647852352, 0.9781476007338056, 0.981627183447664,
  0.984807753012208, 0.9876883405951378, 0.9902680687415703, 0.992546151641322,
  0.9945218953682733, 0.9961946980917455, 0.9975640502598242, 0.9986295347545738,
  0.9993908270190958, 0.9998476951563913, 1.0, 0.9998476951563913,
  0.9993908270190958, 0.9986295347545738, 0.9975640502598242, 0.9961946980917455,
  0.9945218953682734, 0.9925461516413221, 0.9902680687415704, 0.9876883405951377,
  0.984807753012208, 0.981627183447664, 0.9781476007338057, 0.9743700647852352,
  0.9702957262759965, 0.9659258262890683, 0.9612616959383189, 0.9563047559630355,
  0.9510565162951536, 0.9455185755993168, 0.9396926207859084, 0.9335804264972017,
  0.9271838545667874, 0.9205048534524404, 0.913545457642601, 0.90630778703665,
  0.8987940462991669, 0.8910065241883679, 0.8829475928589271, 0.8746197071393959,
  0.8660254037844387, 0.8571673007021123, 0.8480480961564261, 0.838670567945424,
  0.8290375725550417, 0.819152044288992, 0.8090169943749475, 0.7986355100472927,
  0.788010753606722, 0.777145961456971, 0.766044443118978, 0.7547095802227718,
  0.7431448254773942, 0.7313537016191706, 0.7193398003386514, 0.7071067811865476,
  0.6946583704589971, 0.6819983600624986, 0.6691306063588583, 0.6560590289905073,
  0.6427876096865395, 0.6293203910498377, 0.6156614753256584, 0.6018150231520482,
  0.5877852522924732, 0.5735764363510464, 0.5591929034707469, 0.5446390350150269,
  0.5299192642332049, 0.5150380749100544, 0.49999999999999994, 0.48480962024633717,
  0.4694715627858911, 0.45399049973954686, 0.4383711467890773, 0.4226182617406995,
  0.40673664307580043, 0.39073112848927416, 0.37460659341591224, 0.3583679495453002,
  0.3420201433256689, 0.325568154457157, 0.3090169943749475, 0.29237170472273705,
  0.27563735581699966, 0.258819045102521, 0.24192189559966773, 0.22495105434386478,
  0.20791169081775931, 0.19080899537654497, 0.17364817766693028, 0.15643446504023098,
  0.13917310096006574, 0.12186934340514755, 0.10452846326765373, 0.08715574274765864,
  0.06975647374412552, 0.05233595624294381, 0.0348994967025007, 0.01745240643728344,
  1.2246467991473532e-16, -0.017452406437283192, -0.0348994967025009, -0.052335956242943564,
  -0.06975647374412483, -0.08715574274765794, -0.10452846326765305, -0.12186934340514775,
  -0.13917310096006552, -0.15643446504023073, -0.17364817766693047, -0.19080899537654472,
  -0.20791169081775907, -0.22495105434386498, -0.2419218955996675, -0.25881904510252035,
  -0.275637355816999, -0.2923717047227364, -0.30901699437494773, -0.32556815445715676,
  -0.34202014332566866, -0.35836794954530043, -0.374606593415912, -0.39073112848927355,
  -0.4067366430757998, -0.4226182617406993, -0.43837114678907707, -0.45399049973954625,
  -0.46947156278589086, -0.48480962024633695, -0.5000000000000001, -0.5150380749100542,
  -0.5299192642332048, -0.5446390350150271, -0.5591929034707467, -0.5735764363510458,
  -0.587785252292473, -0.601815023152048, -0.6156614753256578, -0.6293203910498376,
  -0.6427876096865393, -0.6560590289905074, -0.6691306063588582, -0.6819983600624984,
  -0.6946583704589974, -0.7071067811865475, -0.7193398003386509, -0.7313537016191701,
  -0.743144825477394, -0.7547095802227717, -0.7660444431189779, -0.7771459614569711,
  -0.7880107536067221, -0.7986355100472928, -0.8090169943749473, -0.8191520442889916,
  -0.8290375725550414, -0.838670567945424, -0.848048096156426, -0.8571673007021121,
  -0.8660254037844385, -0.8746197071393959, -0.882947592858927, -0.8910065241883678,
  -0.8987940462991668, -0.9063077870366497, -0.913545457642601, -0.9205048534524403,
  -0.9271838545667873, -0.9335804264972016, -0.9396926207859082, -0.9455185755993168,
  -0.9510565162951535, -0.9563047559630353, -0.9612616959383189, -0.9659258262890683,
  -0.9702957262759965, -0.9743700647852351, -0.9781476007338056, -0.9816271834476639,
  -0.984807753012208, -0.9876883405951377, -0.9902680687415704, -0.9925461516413221,
  -0.9945218953682734, -0.9961946980917455, -0.9975640502598242, -0.9986295347545738,
  -0.9993908270190957, -0.9998476951563913, -1.0, -0.9998476951563913,
  -0.9993908270190958, -0.9986295347545738, -0.9975640502598243, -0.9961946980917455,
  -0.9945218953682734, -0.992546151641322, -0.9902680687415704, -0.9876883405951378,
  -0.9848077530122081, -0.9816271834476641, -0.9781476007338058, -0.9743700647852352,
  -0.9702957262759966, -0.9659258262890682, -0.9612616959383188, -0.9563047559630354,
  -0.9510565162951536, -0.945518575599317, -0.9396926207859085, -0.9335804264972021,
  -0.9271838545667874, -0.9205048534524405, -0.9135454576426008, -0.9063077870366498,
  -0.898794046299167, -0.891006524188368, -0.8829475928589271, -0.8746197071393961,
  -0.8660254037844386, -0.8571673007021123, -0.8480480961564261, -0.8386705679454243,
  -0.8290375725550421, -0.8191520442889918, -0.8090169943749476, -0.798635510047293,
  -0.7880107536067218, -0.7771459614569708, -0.7660444431189781, -0.7547095802227721,
  -0.7431448254773946, -0.731353701619171, -0.7193398003386517, -0.7071067811865477,
  -0.6946583704589976, -0.6819983600624983, -0.6691306063588581, -0.6560590289905074,
  -0.6427876096865396, -0.6293203910498378, -0.6156614753256588, -0.6018150231520483,
  -0.5877852522924734, -0.5735764363510465, -0.5591929034707473, -0.544639035015027,
  -0.5299192642332058, -0.5150380749100545, -0.5000000000000004, -0.4848096202463369,
  -0.4694715627858908, -0.45399049973954697, -0.438371146789077, -0.4226182617407,
  -0.40673664307580015, -0.3907311284892747, -0.37460659341591235, -0.35836794954530077,
  -0.3420201433256686, -0.32556815445715753, -0.3090169943749476, -0.29237170472273627,
  -0.2756373558169998, -0.2588190451025207, -0.24192189559966787, -0.22495105434386534,
  -0.20791169081775987, -0.19080899537654467, -0.17364817766693127, -0.15643446504023112,
  -0.13917310096006588, -0.12186934340514811, -0.10452846326765342, -0.08715574274765832,
  -0.06975647374412476, -0.05233595624294437, -0.034899496702500823, -0.01745240643728445,
};

/* JS Math.round: halves go up (C round() takes them away from zero) */
static double js_round(double v) {
  double f = floor(v);
  return v - f >= 0.5 ? f + 1.0 : f;
}

/* a product rounded to a double, never fused into a following add */
static double fmul(double a, double b) {
  volatile double p = a * b;
  return p;
}

int lcd_init(lcd_t *l, int w, int h, fonts_t *fonts, const font_class_t *cls) {
  memset(l, 0, sizeof *l);
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return -1;
  l->fb = calloc((size_t)w * (size_t)h, sizeof *l->fb);
  if (!l->fb) return -1;
  l->w = w;
  l->h = h;
  l->row0 = 0;
  l->row1 = h;
  l->fonts = fonts;
  if (cls) l->cls = *cls;
  else font_class_for(&l->cls, w, h, NULL);
  return 0;
}

void lcd_free(lcd_t *l) {
  lcd_set_pool(l, NULL);
  free(l->fb);
  memset(l, 0, sizeof *l);
}

uint16_t lcd_color_of(uint32_t flags) {
  if (flags & LCDF_RGB) return (uint16_t)(flags >> 16);
  uint32_t idx = flags >> 16;
  return idx < LCD_NAMED_COLORS ? NAMED565[idx] : 0;
}

uint16_t lcd_blend565(uint16_t dst, uint16_t src, int a) {
  int r1 = dst >> 11, g1 = (dst >> 5) & 63, b1 = dst & 31;
  int r2 = src >> 11, g2 = (src >> 5) & 63, b2 = src & 31;
  int r = (r2 * a + r1 * (255 - a) + 127) / 255;
  int g = (g2 * a + g1 * (255 - a) + 127) / 255;
  int b = (b2 * a + b1 * (255 - a) + 127) / 255;
  /* out-of-range a: the bits JS's int32 shifts and the Uint16Array store would keep */
  return (uint16_t)(((uint32_t)r << 11) | ((uint32_t)g << 5) | (uint32_t)b);
}

static void fill16(uint16_t *p, uint16_t c, int64_t n) {
  if ((c >> 8) == (c & 0xFF)) { memset(p, c & 0xFF, (size_t)n * 2); return; }
  for (int64_t i = 0; i < n; i++) p[i] = c;
}

/* lcd_blend565 of colour c at alpha a (0..255) for every value of each channel: the channels blend on
 * their own, so a pixel is three lookups instead of three divisions */
typedef struct { uint16_t r[32], g[64], b[32]; } blend_tab_t;

static void blend_tab(blend_tab_t *t, uint16_t c, int a) {
  for (int v = 0; v < 32; v++) {
    t->r[v] = lcd_blend565((uint16_t)(v << 11), c, a) & 0xF800;
    t->b[v] = lcd_blend565((uint16_t)v, c, a) & 0x001F;
  }
  for (int v = 0; v < 64; v++) t->g[v] = lcd_blend565((uint16_t)(v << 5), c, a) & 0x07E0;
}

static void blend_tab_run(uint16_t *p, const blend_tab_t *t, int64_t n) {
  for (int64_t i = 0; i < n; i++) {
    uint16_t d = p[i];
    p[i] = t->r[d >> 11] | t->g[(d >> 5) & 63] | t->b[d & 31];
  }
}

/* a run blended at alpha a (0..255); a long one through the tables */
#define BLEND_TAB_MIN 256
static void blend_run(uint16_t *p, uint16_t c, int64_t n, int a) {
  if (n >= BLEND_TAB_MIN) {
    blend_tab_t t;
    blend_tab(&t, c, a);
    blend_tab_run(p, &t, n);
    return;
  }
  for (int64_t i = 0; i < n; i++) p[i] = lcd_blend565(p[i], c, a);
}

/* ------------------------------------------------------------------ recording (drawing = multi)
 *
 * A recorded call keeps what its drawing needs, after the work that counts it in st (clipping a
 * rect, the ends of a line, a glyph's place), and the rows it can touch. lcd_flush() hands bands of
 * rows to the pool: each plays every call that reaches its band, drawing only the band's rows (row0,
 * row1). The primitives clip their rows to row0..row1 and do the rest as before, so the pixels are
 * the same as drawing at once. */
struct lcd_rec {
  rcmd_t *cmd;
  int n, cap;
  par_t *par;
  int bands;
  bool keep;                       /* drawing = gpu: lcd_flush() leaves the calls for lcd_calls() */
  bool held;                       /* fonts_hold() on: glyphs were recorded */
};

/* a new recorded call, or NULL when not recording: then the caller draws at once (also when there is
 * no room for it: what was recorded is drawn first) */
static rcmd_t *rec_add(lcd_t *l, int op, int64_t ylo, int64_t yhi) {
  struct lcd_rec *r = l->rec;
  if (!r) return NULL;
  if (r->n == r->cap) {
    int cap = r->cap ? r->cap * 2 : 4096;
    rcmd_t *m = cap <= (1 << 22) ? realloc(r->cmd, (size_t)cap * sizeof *m) : NULL;
    if (!m) {
      lcd_play(l);
      return NULL;
    }
    r->cmd = m;
    r->cap = cap;
  }
  rcmd_t *k = &r->cmd[r->n++];
  k->op = (uint8_t)op;
  k->ylo = ylo < 0 ? 0 : ylo > l->h ? l->h : (int)ylo;
  k->yhi = yhi < 0 ? -1 : yhi > l->h - 1 ? l->h - 1 : (int)yhi;
  return k;
}

int lcd_set_pool(lcd_t *l, par_t *par) { return lcd_set_mode(l, par, false); }

int lcd_set_mode(lcd_t *l, par_t *par, bool keep) {
  lcd_play(l);
  if (!par && !keep) {
    if (l->rec) free(l->rec->cmd);
    free(l->rec);
    l->rec = NULL;
    return 0;
  }
  if (!l->rec && !(l->rec = calloc(1, sizeof *l->rec))) return -1;
  l->rec->par = par;
  l->rec->keep = keep;
  /* two bands per thread (when one is held up by another process the others take its share), of 16
   * rows or more */
  int bands = par ? par_threads(par) * 2 : 1;
  if (bands > l->h / 16) bands = l->h / 16;
  l->rec->bands = bands > 1 ? bands : 1;
  return 0;
}

const rcmd_t *lcd_calls(const lcd_t *l, int *n) {
  *n = l->rec ? l->rec->n : 0;
  return l->rec ? l->rec->cmd : NULL;
}

static void rows_fill(lcd_t *l, uint16_t c);
static void hline64(lcd_t *l, int64_t x, int64_t y, int64_t w, uint16_t c, int opa);
static void rect_rows(lcd_t *l, int64_t x, int64_t y, int64_t w, int64_t h, uint16_t c, int opa);
static void line_px(lcd_t *l, int ax, int ay, int bx, int by, uint8_t pat, uint16_t c);
static void tri_draw(lcd_t *l, int ix0, int iy0, int ix1, int iy1, int ix2, int iy2, uint16_t c, int opa);
static void point_px(lcd_t *l, int x, int y, uint16_t c);
static void fcircle_draw(lcd_t *l, int cx, int cy, int r, uint16_t c);
static void circle_draw(lcd_t *l, int cx, int cy, int r, uint16_t c);
static void invert_draw(lcd_t *l, int x, int y, int w, int h);
static void glyph_blit(lcd_t *l, const glyph_t *g, int64_t gx, int64_t gy, uint16_t c);

static void play_band(void *ctx, int part) {
  const lcd_t *src = ctx;
  const struct lcd_rec *r = src->rec;
  lcd_t b = *src;
  b.rec = NULL;
  b.row0 = (int)((int64_t)src->h * part / r->bands);
  b.row1 = (int)((int64_t)src->h * (part + 1) / r->bands);
  for (int i = 0; i < r->n; i++) {
    const rcmd_t *k = &r->cmd[i];
    if (k->yhi < b.row0 || k->ylo >= b.row1) continue;
    switch (k->op) {
      case RC_CLEAR: rows_fill(&b, k->c); break;
      case RC_HLINE: hline64(&b, k->u.r.x, k->u.r.y, k->u.r.w, k->c, k->opa); break;
      case RC_RECT: rect_rows(&b, k->u.r.x, k->u.r.y, k->u.r.w, k->u.r.h, k->c, k->opa); break;
      case RC_LINE: line_px(&b, k->u.p.x0, k->u.p.y0, k->u.p.x1, k->u.p.y1, k->pat, k->c); break;
      case RC_TRI: tri_draw(&b, k->u.p.x0, k->u.p.y0, k->u.p.x1, k->u.p.y1, k->u.p.x2, k->u.p.y2, k->c, k->opa); break;
      case RC_POINT: point_px(&b, k->u.p.x0, k->u.p.y0, k->c); break;
      case RC_FCIRCLE: fcircle_draw(&b, k->u.p.x0, k->u.p.y0, k->u.p.x1, k->c); break;
      case RC_CIRCLE: circle_draw(&b, k->u.p.x0, k->u.p.y0, k->u.p.x1, k->c); break;
      case RC_INVERT: invert_draw(&b, k->u.p.x0, k->u.p.y0, k->u.p.x1, k->u.p.y1); break;
      case RC_GLYPH: glyph_blit(&b, &k->u.gl.g, k->u.gl.x, k->u.gl.y, k->c); break;
    }
  }
}

void lcd_flush(lcd_t *l) {
  if (l->rec && !l->rec->keep) lcd_play(l);
}

void lcd_play(lcd_t *l) {
  struct lcd_rec *r = l->rec;
  if (!r || !r->n) return;
  par_run(r->par, r->bands, play_band, l);
  lcd_drop(l);
}

void lcd_drop(lcd_t *l) {
  struct lcd_rec *r = l->rec;
  if (!r) return;
  r->n = 0;
  if (r->held) {
    fonts_hold(l->fonts, false);
    r->held = false;
  }
}

/* the rows this lcd_t draws, in one colour */
static void rows_fill(lcd_t *l, uint16_t c) {
  fill16(l->fb + (size_t)l->row0 * (size_t)l->w, c, (int64_t)(l->row1 - l->row0) * l->w);
}

void lcd_clear(lcd_t *l, uint16_t c) {
  l->st.rects++;
  if (l->rec) l->rec->n = 0;                  /* what was recorded would be painted over */
  rcmd_t *k = rec_add(l, RC_CLEAR, 0, l->h - 1);
  if (k) {
    k->c = c;
    return;
  }
  rows_fill(l, c);
}

/* engine.js hline; opacity 0..15 like EdgeTX (0 = opaque) */
static void hline64(lcd_t *l, int64_t x, int64_t y, int64_t w, uint16_t c, int opa) {
  if (y < l->row0 || y >= l->row1 || w <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (x + w > l->w) w = l->w - x;
  if (w <= 0) return;
  uint16_t *p = l->fb + y * l->w + x;
  if (!opa) fill16(p, c, w);
  else blend_run(p, c, w, (15 - opa) * 17);   /* Math.round((15 - opa) * 255 / 15) */
}

void lcd_hline(lcd_t *l, int x, int y, int w, uint16_t c, int opa) {
  rcmd_t *k = rec_add(l, RC_HLINE, y, y);
  if (k) {
    k->u.r.x = x;
    k->u.r.y = y;
    k->u.r.w = w;
    k->c = c;
    k->opa = (uint8_t)(opa & 15);
    return;
  }
  hline64(l, x, y, w, c, opa & 15);
}

/* engine.js fillRect: negative sizes flip, then clip */
static void fill_rect64(lcd_t *l, int64_t x, int64_t y, int64_t w, int64_t h, uint16_t c, int opa) {
  if (opa >= 15) return;
  if (w < 0) { x += w; w = -w; }
  if (h < 0) { y += h; h = -h; }
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > l->w) w = l->w - x;
  if (y + h > l->h) h = l->h - y;
  if (w <= 0 || h <= 0) return;
  l->st.rects++;
  rcmd_t *k = rec_add(l, RC_RECT, y, y + h - 1);
  if (k) {
    k->u.r.x = x;
    k->u.r.y = y;
    k->u.r.w = w;
    k->u.r.h = h;
    k->c = c;
    k->opa = (uint8_t)opa;
    return;
  }
  rect_rows(l, x, y, w, h, c, opa);
}

/* a rect on the screen (fill_rect64 clipped it): its rows in row0..row1 */
static void rect_rows(lcd_t *l, int64_t x, int64_t y, int64_t w, int64_t h, uint16_t c, int opa) {
  int64_t j0 = y > l->row0 ? y : l->row0, j1 = y + h < l->row1 ? y + h : l->row1;
  if (j0 >= j1) return;
  if (opa && w * (j1 - j0) >= BLEND_TAB_MIN) {
    blend_tab_t t;
    blend_tab(&t, c, (15 - opa) * 17);
    for (int64_t j = j0; j < j1; j++) blend_tab_run(l->fb + j * l->w + x, &t, w);
    return;
  }
  for (int64_t j = j0; j < j1; j++) {
    uint16_t *p = l->fb + j * l->w + x;
    if (!opa) fill16(p, c, w);
    else blend_run(p, c, w, (15 - opa) * 17);
  }
}

/* The Lua API masks opacities with & 15 before they get here; doing it again is harmless and
 * keeps the primitives safe for other callers. */
void lcd_fill_rect(lcd_t *l, int x, int y, int w, int h, uint16_t c, int opa) {
  fill_rect64(l, x, y, w, h, c, opa & 15);
}

void lcd_point(lcd_t *l, int x, int y, uint16_t c) {
  rcmd_t *k = rec_add(l, RC_POINT, y, y);
  if (k) {
    k->u.p.x0 = x;
    k->u.p.y0 = y;
    k->c = c;
    return;
  }
  point_px(l, x, y, c);
}

static void point_px(lcd_t *l, int x, int y, uint16_t c) {
  if (x >= 0 && y >= l->row0 && x < l->w && y < l->row1) l->fb[(size_t)y * l->w + x] = c;
}

/* Liang-Barsky in doubles, like engine.js */
static bool lb_clip(double p, double q, double *t0, double *t1) {
  if (p == 0) return q >= 0;
  double r = q / p;
  if (p < 0) {
    if (r > *t1) return false;
    if (r > *t0) *t0 = r;
  } else {
    if (r < *t0) return false;
    if (r < *t1) *t1 = r;
  }
  return true;
}

/* EdgeTX BitmapBuffer::drawLine: clip, round the ends, Bresenham with an 8-bit pattern */
void lcd_line(lcd_t *l, int x1, int y1, int x2, int y2, uint8_t pat, uint16_t c) {
  const int W = l->w, H = l->h;
  l->st.lines++;
  double t0 = 0, t1 = 1;
  double dx = (double)x2 - x1, dy = (double)y2 - y1;
  if (!lb_clip(-dx, x1, &t0, &t1) || !lb_clip(dx, (double)(W - 1) - x1, &t0, &t1) ||
      !lb_clip(-dy, y1, &t0, &t1) || !lb_clip(dy, (double)(H - 1) - y1, &t0, &t1)) return;
  /* the clipped ends lie on the screen (to within rounding), so they fit an int */
  int ax = (int)js_round(x1 + fmul(t0, dx)), ay = (int)js_round(y1 + fmul(t0, dy));
  int bx = (int)js_round(x1 + fmul(t1, dx)), by = (int)js_round(y1 + fmul(t1, dy));
  rcmd_t *k = rec_add(l, RC_LINE, ay < by ? ay : by, ay < by ? by : ay);
  if (k) {
    k->u.p.x0 = ax;
    k->u.p.y0 = ay;
    k->u.p.x1 = bx;
    k->u.p.y1 = by;
    k->pat = pat;
    k->c = c;
    return;
  }
  line_px(l, ax, ay, bx, by, pat, c);
}

/* Bresenham from the clipped, rounded ends; pixels in row0..row1 */
static void line_px(lcd_t *l, int ax, int ay, int bx, int by, uint8_t pat, uint16_t c) {
  const int W = l->w, Y0 = l->row0, Y1 = l->row1;
  int ddx = bx - ax, ddy = by - ay;
  int adx = abs(ddx), ady = abs(ddy), sx = (ddx > 0) - (ddx < 0), sy = (ddy > 0) - (ddy < 0);
  int ex = ady >> 1, ey = adx >> 1, px = ax, py = ay;
  if (!ady && pat == 0xFF) {
    /* solid and level: the same pixels as below, as one run */
    if (py < Y0 || py >= Y1) return;
    int lo = ax < bx ? ax : bx, hi = ax < bx ? bx : ax;
    if (lo < 0) lo = 0;
    if (hi > W - 1) hi = W - 1;
    if (lo <= hi) fill16(l->fb + (size_t)py * W + lo, c, hi - lo + 1);
    return;
  }
  /* A band draws only the steps that land in its rows, starting at the first of them: after k steps the
   * minor coordinate has moved floor((major / 2 + k * minor) / major) times and the error term is the
   * remainder, as the loop would have it. */
  if (adx >= ady) {
    int64_t k0 = 0, k1 = adx, h0 = adx >> 1;
    if (ady) {
      /* py = ay + sy * floor((h0 + k * ady) / adx): that floor must be in D..E */
      int64_t D = sy > 0 ? Y0 - ay : ay - (Y1 - 1), E = sy > 0 ? Y1 - 1 - ay : ay - Y0;
      if (E < 0 || D > ady) return;
      if (D > 0) k0 = (D * adx - h0 + ady - 1) / ady;
      int64_t last = ((E + 1) * adx - 1 - h0) / ady;
      if (last < k1) k1 = last;
    } else if (ay < Y0 || ay >= Y1) {
      return;
    }
    if (k0 > k1) return;
    int64_t q = h0 + k0 * ady;
    px = ax + sx * (int)k0;
    py = ay + (adx ? sy * (int)(q / adx) : 0);
    ey = adx ? (int)(q % adx) : 0;
    for (int64_t i = k0; i <= k1; i++) {
      if (((1 << (px & 7)) & pat) && px >= 0 && px < W && py >= Y0 && py < Y1) l->fb[(size_t)py * W + px] = c;
      ey += ady;
      if (ey >= adx) { ey -= adx; py += sy; }
      px += sx;
    }
  } else {
    /* py = ay + sy * k */
    int64_t k0 = sy > 0 ? Y0 - ay : ay - (Y1 - 1), k1 = sy > 0 ? Y1 - 1 - ay : ay - Y0;
    if (k0 < 0) k0 = 0;
    if (k1 > ady) k1 = ady;
    if (k0 > k1) return;
    int64_t q = (int64_t)(ady >> 1) + k0 * adx;
    py = ay + sy * (int)k0;
    px = ax + sx * (int)(q / ady);
    ex = (int)(q % ady);
    for (int64_t i = k0; i <= k1; i++) {
      if (((1 << (py & 7)) & pat) && px >= 0 && px < W && py >= Y0 && py < Y1) l->fb[(size_t)py * W + px] = c;
      ex += adx;
      if (ex >= ady) { ex -= ady; px += sx; }
      py += sy;
    }
  }
}

/* luaLcdDrawLine: an end beyond the right or bottom edge (x > LCD_W, y > LCD_H) draws nothing */
void lcd_draw_line_api(lcd_t *l, int x1, int y1, int x2, int y2, int pat, uint32_t flags) {
  if (x1 > l->w || y1 > l->h || x2 > l->w || y2 > l->h) {
    l->st.lines++;
    l->st.rejected++;
    return;
  }
  lcd_line(l, x1, y1, x2, y2, (uint8_t)(pat & 255), lcd_color_of(flags));
}

static void tri_row(lcd_t *l, int64_t a, int64_t b, int64_t y, uint16_t c, int opa) {
  if (a > b) { int64_t q = a; a = b; b = q; }
  hline64(l, a, y, b - a + 1, c, opa);
}

/* Adafruit GFX fill as in BitmapBuffer::drawFilledTriangle: one row per y, truncating division.
 * engine.js accumulates sa/sb row by row; the product dx * (y - y0) is the same number (exact
 * below 2^53, which any triangle a game could draw stays under) and lets the loops start at the
 * first visible row. */
void lcd_tri(lcd_t *l, int ix0, int iy0, int ix1, int iy1, int ix2, int iy2, uint16_t c, int opa) {
  opa &= 15;
  l->st.tris++;
  int lo = iy0 < iy1 ? iy0 : iy1, hi = iy0 > iy1 ? iy0 : iy1;
  rcmd_t *k = rec_add(l, RC_TRI, iy2 < lo ? iy2 : lo, iy2 > hi ? iy2 : hi);
  if (k) {
    k->u.p.x0 = ix0;
    k->u.p.y0 = iy0;
    k->u.p.x1 = ix1;
    k->u.p.y1 = iy1;
    k->u.p.x2 = ix2;
    k->u.p.y2 = iy2;
    k->c = c;
    k->opa = (uint8_t)opa;
    return;
  }
  tri_draw(l, ix0, iy0, ix1, iy1, ix2, iy2, c, opa);
}

static void tri_draw(lcd_t *l, int ix0, int iy0, int ix1, int iy1, int ix2, int iy2, uint16_t c, int opa) {
  int64_t x0 = ix0, y0 = iy0, x1 = ix1, y1 = iy1, x2 = ix2, y2 = iy2, t;
  const int64_t ymax = l->row1 - 1;
  if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
  if (y1 > y2) { t = y2; y2 = y1; y1 = t; t = x2; x2 = x1; x1 = t; }
  if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
  if (y0 == y2) {
    int64_t a = x0, b = x0;
    if (x1 < a) a = x1;
    if (x2 < a) a = x2;
    if (x1 > b) b = x1;
    if (x2 > b) b = x2;
    tri_row(l, a, b, y0, c, opa);
    return;
  }
  const double dx01 = (double)(x1 - x0), dy01 = (double)(y1 - y0), dx02 = (double)(x2 - x0);
  const double dy02 = (double)(y2 - y0), dx12 = (double)(x2 - x1), dy12 = (double)(y2 - y1);
  const int64_t last = y1 == y2 ? y1 : y1 - 1;
  int64_t y = y0 > l->row0 ? y0 : l->row0, yend = last < ymax ? last : ymax;
  for (; y <= yend; y++) {
    double sa = (double)(y - y0) * dx01, sb = (double)(y - y0) * dx02;
    tri_row(l, x0 + (int64_t)trunc(sa / dy01), x0 + (int64_t)trunc(sb / dy02), y, c, opa);
  }
  y = last + 1;
  if (y < l->row0) y = l->row0;
  yend = y2 < ymax ? y2 : ymax;
  for (; y <= yend; y++) {
    double sa = (double)(y - y1) * dx12, sb = (double)(y - y0) * dx02;
    tri_row(l, x1 + (int64_t)trunc(sa / dy12), x0 + (int64_t)trunc(sb / dy02), y, c, opa);
  }
}

void lcd_filled_circle(lcd_t *l, int cx, int cy, int r, uint16_t c) {
  rcmd_t *k = rec_add(l, RC_FCIRCLE, (int64_t)cy - r, (int64_t)cy + r);
  if (k) {
    k->u.p.x0 = cx;
    k->u.p.y0 = cy;
    k->u.p.x1 = r;
    k->c = c;
    return;
  }
  fcircle_draw(l, cx, cy, r, c);
}

static void fcircle_draw(lcd_t *l, int cx, int cy, int r, uint16_t c) {
  int64_t lo = -(int64_t)r, hi = r;
  if (lo < (int64_t)l->row0 - cy) lo = (int64_t)l->row0 - cy;
  if (hi > (int64_t)l->row1 - 1 - cy) hi = (int64_t)l->row1 - 1 - cy;
  for (int64_t dy = lo; dy <= hi; dy++) {
    int64_t dx = (int64_t)floor(sqrt(fmul(r, r) - fmul((double)dy, (double)dy)));
    hline64(l, (int64_t)cx - dx, cy + dy, dx * 2 + 1, c, 0);
  }
}

void lcd_circle(lcd_t *l, int cx, int cy, int r, uint16_t c) {
  int64_t ar = r < 0 ? -(int64_t)r : r;
  rcmd_t *k = rec_add(l, RC_CIRCLE, (int64_t)cy - ar - 1, (int64_t)cy + ar + 1);
  if (k) {
    k->u.p.x0 = cx;
    k->u.p.y0 = cy;
    k->u.p.x1 = r;
    k->c = c;
    return;
  }
  circle_draw(l, cx, cy, r, c);
}

static void circle_draw(lcd_t *l, int cx, int cy, int r, uint16_t c) {
  for (int a = 0; a < 360; a++) {
    double x = js_round(cx + fmul(r, COS[a])), y = js_round(cy + fmul(r, SIN[a]));
    if (x >= 0 && y >= l->row0 && x < l->w && y < l->row1) l->fb[(size_t)y * l->w + (size_t)x] = c;
  }
}

void lcd_invert_rect(lcd_t *l, int x, int y, int w, int h) {
  rcmd_t *k = rec_add(l, RC_INVERT, y, (int64_t)y + h - 1);
  if (k) {
    k->u.p.x0 = x;
    k->u.p.y0 = y;
    k->u.p.x1 = w;
    k->u.p.y1 = h;
    return;
  }
  invert_draw(l, x, y, w, h);
}

static void invert_draw(lcd_t *l, int x, int y, int w, int h) {
  int64_t j0 = y > l->row0 ? y : l->row0, j1 = (int64_t)y + h, i0 = x > 0 ? x : 0, i1 = (int64_t)x + w;
  if (j1 > l->row1) j1 = l->row1;
  if (i1 > l->w) i1 = l->w;
  for (int64_t j = j0; j < j1; j++) {
    uint16_t *p = l->fb + j * l->w;
    for (int64_t i = i0; i < i1; i++) p[i] ^= 0xFFFF;
  }
}

void lcd_draw_rectangle(lcd_t *l, int x, int y, int w, int h, uint32_t flags, int t, int opa) {
  uint16_t c = lcd_color_of(flags);
  int64_t X = x, Y = y, Wd = w, Ht = h, T = t;
  opa &= 15;
  fill_rect64(l, X, Y, T, Ht, c, opa);
  fill_rect64(l, X + Wd - T, Y, T, Ht, c, opa);
  fill_rect64(l, X + T, Y, Wd - 2 * T, T, c, opa);
  fill_rect64(l, X + T, Y + Ht - T, Wd - 2 * T, T, c, opa);
}

/* JS Math.min / Math.max: NaN wins */
static double js_min(double a, double b) { return a != a || b != b ? NAN : a < b ? a : b; }
static double js_max(double a, double b) { return a != a || b != b ? NAN : a > b ? a : b; }

void lcd_draw_gauge(lcd_t *l, int x, int y, int w, int h, int n, int d, uint32_t flags) {
  fill_rect64(l, x, y, w, h, NAMED565[10], 0);
  /* n / 0 is +-Infinity (full / empty); 0 / 0 is NaN: JS then fills up to a NaN end, nothing */
  double f = js_round((double)w * js_max(0, js_min(1, (double)n / (double)d)));
  if (f != f) return;
  fill_rect64(l, x, y, (int64_t)f, h, lcd_color_of(flags), 0);
}

/* ------------------------------------------------------------------ text */

static font_face_t *text_face(lcd_t *l, uint32_t flags) {
  int i = (flags >> 8) & 0xF, px = 16;
  bool bold = false;
  if (i < FONT_SIZES) {
    if (l->cls.px[i]) px = l->cls.px[i];   /* FONT_PX[cls][i] || 16 */
    bold = FONT_BOLD[i];
  }
  return fonts_face(l->fonts, px, bold);
}

static void put_text(lcd_t *l, font_face_t *face, const char *s, int len, int64_t ox, int64_t oy, uint16_t c) {
  double cx = (double)ox;
  for (int i = 0; i < len;) {
    const glyph_t *g = font_glyph(l->fonts, face, font_utf8_next(s, len, &i));
    l->st.glyphs++;
    if (g->a) {
      int64_t gx = (int64_t)js_round(cx + g->ox), gy = oy + g->oy;
      rcmd_t *k = rec_add(l, RC_GLYPH, gy, gy + g->h - 1);
      if (k) {
        k->u.gl.x = gx;
        k->u.gl.y = gy;
        k->u.gl.g = *g;
        k->c = c;
        if (!l->rec->held) {                   /* the mask must outlive the font cache's evictions */
          fonts_hold(l->fonts, true);
          l->rec->held = true;
        }
      } else {
        glyph_blit(l, g, gx, gy, c);
      }
    }
    cx += g->adv;
  }
}

static void glyph_blit(lcd_t *l, const glyph_t *g, int64_t gx, int64_t gy, uint16_t c) {
  const int W = l->w;
  for (int j = 0; j < g->h; j++) {
    int64_t yy = gy + j;
    if (yy < l->row0 || yy >= l->row1) continue;
    const uint8_t *m = g->a + j * g->w;
    uint16_t *row = l->fb + yy * W;
    for (int i = 0; i < g->w; i++) {
      int a = m[i];
      if (!a) continue;
      int64_t xx = gx + i;
      if (xx < 0 || xx >= W) continue;
      row[xx] = a >= 250 ? c : lcd_blend565(row[xx], c, a);
    }
  }
}

/* engine.js drawStr */
void lcd_draw_text(lcd_t *l, int x, int y, const char *s, int len, uint32_t flags, int *mw, int *mh) {
  int w = 0, h = 0;
  if (!s) { s = ""; len = 0; }
  if (len < 0) len = (int)strlen(s);
  if (!l->fonts) {
    if (mw) *mw = 0;
    if (mh) *mh = 0;
    return;
  }
  font_face_t *face = text_face(l, flags);
  font_measure(l->fonts, face, s, len, &w, &h);
  double fx = x, fy = y;
  if (flags & LCDF_RIGHT) fx -= w;
  else if (flags & LCDF_CENTER) fx -= w / 2.0;
  if (flags & LCDF_VCENTER) fy -= h / 2.0;
  int64_t X = (int64_t)js_round(fx), Y = (int64_t)js_round(fy);
  l->st.texts++;
  if (flags & LCDF_INVERS) fill_rect64(l, X - 1, Y, (int64_t)w + 2, h, lcd_color_of(flags), 0);
  if (flags & LCDF_SHADOWED) put_text(l, face, s, len, X + 1, Y + 1, 0);
  put_text(l, face, s, len, X, Y, (flags & LCDF_INVERS) ? 0xFFFF : lcd_color_of(flags));
  if (mw) *mw = w;
  if (mh) *mh = h;
}

/* engine.js numStr: (v / 10).toFixed(1), (v / 100).toFixed(2), else String(v). For an int32 v
 * the double v / 10^p is within 1e-7 of the exact value, so toFixed always prints v's own
 * digits: integer formatting gives the same string. */
void lcd_draw_number(lcd_t *l, int x, int y, int v, uint32_t flags) {
  char buf[32];
  uint32_t prec = flags & 0x30;
  if (prec == LCDF_PREC1 || prec == LCDF_PREC2) {
    int digits = prec == LCDF_PREC1 ? 1 : 2, div = prec == LCDF_PREC1 ? 10 : 100;
    long long a = v < 0 ? -(long long)v : v;
    snprintf(buf, sizeof buf, "%s%lld.%0*lld", v < 0 ? "-" : "", a / div, digits, a % div);
  } else {
    snprintf(buf, sizeof buf, "%d", v);
  }
  lcd_draw_text(l, x, y, buf, (int)strlen(buf), flags, NULL, NULL);
}

/* engine.js drawTimer: [-]MM:SS with minutes past 99 as they are (TIMEHOUR is ignored there too) */
void lcd_draw_timer(lcd_t *l, int x, int y, int v, uint32_t flags) {
  char buf[32];
  long long a = v < 0 ? -(long long)v : v;
  snprintf(buf, sizeof buf, "%s%02lld:%02lld", v < 0 ? "-" : "", a / 60, a % 60);
  lcd_draw_text(l, x, y, buf, (int)strlen(buf), flags, NULL, NULL);
}

void lcd_size_text(lcd_t *l, const char *s, int len, uint32_t flags, int *w, int *h) {
  int mw = 0, mh = 0, i = (flags >> 8) & 0xF;
  if (!s) { s = ""; len = 0; }
  if (len < 0) len = (int)strlen(s);
  if (l->fonts) font_measure(l->fonts, text_face(l, flags), s, len, &mw, &mh);
  if (w) *w = mw;
  /* LINE_H[cls][i] || m.h */
  if (h) *h = i < FONT_SIZES && l->cls.line_h[i] ? l->cls.line_h[i] : mh;
}

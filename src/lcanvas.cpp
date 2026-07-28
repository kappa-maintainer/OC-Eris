#define LUA_LIB
#include "lualib.h"
#include "llimits.h" // l_unlikely

#include "vendor/Soup/soup/Canvas.hpp"
#include "vendor/Soup/soup/MemoryRefReader.hpp"
#include "vendor/Soup/soup/QrCode.hpp"

static soup::Canvas* checkcanvas (lua_State *L, int i) {
  return (soup::Canvas*)luaL_checkudata(L, i, "pluto:canvas");
}

static int canvas_dump (lua_State* L);

static void pushcanvas (lua_State *L, soup::Canvas&& canvas) {
  new (lua_newuserdata(L, sizeof(soup::Canvas))) soup::Canvas(std::move(canvas));
  if (luaL_newmetatable(L, "pluto:canvas")) {
    lua_pushliteral(L, "__index");
    /* Fetch the library table straight from the registry instead of going
     * through require, which embedders that omit the package library do not
     * have. When the host opened this library the cached table is the very one
     * it exposed, so methods stay identical to the public functions. */
    luaL_requiref(L, PLUTO_CANVASLIBNAME, luaopen_canvas, 0);
    lua_settable(L, -3);
    lua_pushliteral(L, "__gc");
    lua_pushcfunction(L, [](lua_State *L) {
      pluto_errorifnotgc(L);
      std::destroy_at<>(checkcanvas(L, 1));
      return 0;
    });
    lua_settable(L, -3);
    /* Persist through the exact pixel dump rather than an image codec. */
    pluto_setpersist(L, canvas_dump, PLUTO_CANVASLIBNAME, "undump");
  }
  lua_setmetatable(L, -2);
}

static int canvas_new (lua_State* L) {
  const auto width = luaL_checkinteger(L, 1);
  const auto height = luaL_checkinteger(L, 2);
  pushcanvas(L, soup::Canvas(static_cast<unsigned int>(width), static_cast<unsigned int>(height)));
  return 1;
}

static int canvas_bmp (lua_State *L) {
  size_t size;
  const char *data = luaL_checklstring(L, 1, &size);
  soup::MemoryRefReader r(data, size);
  pushcanvas(L, soup::Canvas::fromBmp(r));
  return 1;
}

static int canvas_qrcode (lua_State *L) {
  size_t size;
  const char *data = luaL_checklstring(L, 1, &size);
  soup::QrCode::ecc ecl = soup::QrCode::ecc::LOW;
  unsigned int border = 0;
  soup::Rgb fg = soup::Rgb::BLACK;
  soup::Rgb bg = soup::Rgb::WHITE;
  if (lua_gettop(L) >= 2) {
    luaL_checktype(L, 2, LUA_TTABLE);

    lua_pushliteral(L, "ecl");
    if (lua_gettable(L, 2) > LUA_TNIL) {
      const char* const options[] = { "low", "medium", "quartile", "high", nullptr };
      ecl = static_cast<soup::QrCode::ecc>(luaL_checkoption(L, -1, "low", options));
    }

    lua_pushliteral(L, "border");
    if (lua_gettable(L, 2) > LUA_TNIL)
      border = static_cast<unsigned int>(luaL_checkinteger(L, -1));

    lua_pushliteral(L, "fg");
    if (lua_gettable(L, 2) > LUA_TNIL)
      fg = soup::Rgb(static_cast<uint32_t>(luaL_checkinteger(L, -1)));

    lua_pushliteral(L, "bg");
    if (lua_gettable(L, 2) > LUA_TNIL)
      bg = soup::Rgb(static_cast<uint32_t>(luaL_checkinteger(L, -1)));
  }

  bool fail = false;
  try {
    std::string content(data, size);
    pushcanvas(L, soup::QrCode::encodeText(content, ecl).toCanvas(border, fg, bg));
  }
  catch (...) {
    fail = true;
  }
  if (l_unlikely(fail))
    luaL_error(L, "failed to encode qrcode");
  return 1;
}

static int canvas_get (lua_State* L) {
  const auto c = checkcanvas(L, 1);
  const auto x = static_cast<unsigned int>(luaL_checkinteger(L, 2));
  const auto y = static_cast<unsigned int>(luaL_checkinteger(L, 3));
  if (l_unlikely(x >= c->width || y >= c->height)) {
    luaL_error(L, "out of bounds");
  }
  lua_pushinteger(L, c->get(x, y).toInt());
  return 1;
}

static int canvas_set (lua_State* L) {
  const auto c = checkcanvas(L, 1);
  const auto x = static_cast<unsigned int>(luaL_checkinteger(L, 2));
  const auto y = static_cast<unsigned int>(luaL_checkinteger(L, 3));
  const auto v = soup::Rgb(static_cast<uint32_t>(luaL_checkinteger(L, 4)));
  if (l_unlikely(x >= c->width || y >= c->height)) {
    luaL_error(L, "out of bounds");
  }
  c->set(x, y, v);
  return 0;
}

static int canvas_fill (lua_State* L) {
  const auto c = checkcanvas(L, 1);
  const auto v = soup::Rgb(static_cast<uint32_t>(luaL_checkinteger(L, 2)));
  c->fill(v);
  return 0;
}

static int canvas_size (lua_State *L) {
  const auto c = checkcanvas(L, 1);
  lua_pushinteger(L, c->width);
  lua_pushinteger(L, c->height);
  return 2;
}

static int canvas_mulsize (lua_State *L) {
  const auto c = checkcanvas(L, 1);
  const auto x = static_cast<unsigned int>(luaL_checkinteger(L, 2));
  if (l_unlikely(x < 2))
    luaL_error(L, "multiplier must be at least 2");
  c->resizeNearestNeighbour(c->width * x, c->height * x);
  return 0;
}

static int canvas_tobmp (lua_State* L) {
  pluto_pushstring(L, checkcanvas(L, 1)->toBmp());
  return 1;
}

static int canvas_topng (lua_State* L) {
  pluto_pushstring(L, checkcanvas(L, 1)->toPng());
  return 1;
}

static int canvas_tobwstring(lua_State* L) {
  pluto_pushstring(L, checkcanvas(L, 1)->toStringDownsampledDoublewidthUtf8(true, false, soup::Rgb(static_cast<uint32_t>(luaL_checkinteger(L, 2)))));
  return 1;
}

/*
** Dump the canvas as its exact pixel data, prefixed by the dimensions.
**
** This exists for __persist rather than for scripts. The image codecs cannot be
** used for that: tobmp/bmp is not a lossless round trip, since Soup's BMP
** reader and writer disagree on channel order and swap red with blue. Writing
** the pixels out verbatim keeps save and load exact and does not depend on any
** codec staying faithful.
*/
static int canvas_dump (lua_State* L) {
  const auto c = checkcanvas(L, 1);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  /* Dimensions as four bytes each, little endian, then three bytes per pixel. */
  for (int i = 0; i != 4; ++i)
    luaL_addchar(&b, static_cast<char>((c->width >> (i * 8)) & 0xFF));
  for (int i = 0; i != 4; ++i)
    luaL_addchar(&b, static_cast<char>((c->height >> (i * 8)) & 0xFF));
  for (const auto& px : c->pixels) {
    luaL_addchar(&b, static_cast<char>(px.r));
    luaL_addchar(&b, static_cast<char>(px.g));
    luaL_addchar(&b, static_cast<char>(px.b));
  }
  luaL_pushresult(&b);
  return 1;
}

/* Inverse of canvas_dump. */
static int canvas_undump (lua_State* L) {
  size_t size;
  const char *data = luaL_checklstring(L, 1, &size);
  luaL_argcheck(L, size >= 8, 1, "truncated canvas data");
  const auto rd = [data](size_t at) {
    return static_cast<unsigned int>(
        (static_cast<unsigned char>(data[at])) |
        (static_cast<unsigned char>(data[at + 1]) << 8) |
        (static_cast<unsigned char>(data[at + 2]) << 16) |
        (static_cast<unsigned char>(data[at + 3]) << 24));
  };
  const unsigned int width = rd(0), height = rd(4);
  /* Guard against a hostile or corrupt blob claiming a size it does not carry.
   * Check the pixel count before multiplying so the product cannot overflow. */
  luaL_argcheck(L, height == 0 || width <= (size - 8) / 3 / height, 1,
                "canvas dimensions do not match data");
  luaL_argcheck(L, size - 8 == (size_t)width * height * 3, 1,
                "canvas dimensions do not match data");
  soup::Canvas c(width, height);
  for (size_t i = 0; i != c.pixels.size(); ++i) {
    c.pixels[i].r = static_cast<uint8_t>(data[8 + i * 3]);
    c.pixels[i].g = static_cast<uint8_t>(data[9 + i * 3]);
    c.pixels[i].b = static_cast<uint8_t>(data[10 + i * 3]);
  }
  pushcanvas(L, std::move(c));
  return 1;
}

static const luaL_Reg funcs_canvas[] = {
  {"new", canvas_new},
  {"bmp", canvas_bmp},
  {"qrcode", canvas_qrcode},
  {"get", canvas_get},
  {"set", canvas_set},
  {"fill", canvas_fill},
  {"size", canvas_size},
  {"mulsize", canvas_mulsize},
  {"tobmp", canvas_tobmp},
  {"topng", canvas_topng},
  {"tobwstring", canvas_tobwstring},
  {"dump", canvas_dump},
  {"undump", canvas_undump},
  {nullptr, nullptr}
};
PLUTO_NEWLIB(canvas);

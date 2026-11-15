#include "qml_override.h"

#include "miniz.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

static const char k_old[] = "source: blckEdt.visible ? curModuleUIFile.translator.string + \".qml\" : \"\"";
static const char k_key[] = "/Pages/BlockDetails.qml";

static uint32_t rd_be32(const uint8_t* p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr_be32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* drops full-line // comments, trailing blanks and empty lines */
static size_t minify(const char* in, size_t n, char* out)
{
  size_t o = 0, i = 0;
  while (i < n)
  {
    size_t e = i;
    while (e < n && in[e] != '\n')
      ++e;
    size_t s = i, t = e;
    while (s < t && (in[s] == ' ' || in[s] == '\t'))
      ++s;
    while (t > s && (in[t - 1] == ' ' || in[t - 1] == '\t' || in[t - 1] == '\r'))
      --t;
    const bool comment = t - s >= 2 && in[s] == '/' && in[s + 1] == '/';
    if (t > s && !comment)
    {
      size_t keep = i; /* keep the leading indentation, drop only the trailing blanks */
      memcpy(out + o, in + keep, t - keep);
      o += t - keep;
      out[o++] = '\n';
    }
    i = e + 1;
  }
  return o;
}

static const uint8_t* find_mem(const uint8_t* h, size_t hl, const uint8_t* n, size_t nl, const uint8_t* from)
{
  if (nl == 0 || hl < nl)
    return NULL;
  const uint8_t* end = h + hl - nl;
  for (const uint8_t* p = from ? from : h; p <= end; ++p)
  {
    p = memchr(p, n[0], (size_t)(end - p) + 1);
    if (!p)
      return NULL;
    if (memcmp(p, n, nl) == 0)
      return p;
  }
  return NULL;
}

static bool break_key(uint8_t* data, size_t data_len, const char* key, char* err, size_t err_size)
{
  const size_t kl = strlen(key);
  uint8_t pat[16 + 2 * 96];
  if (kl > 95)
    return false;
  const uint8_t head[16] = {0xff, 0xff, 0xff, 0xff, (uint8_t)kl, 0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0};
  memcpy(pat, head, 16);
  for (size_t i = 0; i < kl; ++i)
    pat[16 + 2 * i] = (uint8_t)key[i], pat[17 + 2 * i] = 0;
  const uint8_t* k = find_mem(data, data_len, pat, 16 + 2 * kl, NULL);
  if (!k)
  {
    /* a file gets several edits in a row: the key may be broken already (by this very patcher) */
    pat[16 + 2 * (kl - 1)] = 'x';
    const uint8_t* b = find_mem(data, data_len, pat, 16 + 2 * kl, NULL);
    if (b && !find_mem(data, data_len, pat, 16 + 2 * kl, b + 1))
      return true;
  }
  if (!k || find_mem(data, data_len, pat, 16 + 2 * kl, k + 1))
  {
    set_err(err, err_size, "unit key \"%s\" not found exactly once", key);
    return false;
  }
  data[(size_t)(k - data) + 16 + 2 * (kl - 1)] = 'x';
  return true;
}

/* Replaces the one occurrence of `old` in the zlib-compressed (qCompress) source entry that contains it, recompresses into
 * the same slot and breaks the unit key `key`. */
static bool patch_zlib_entry(uint8_t* data, size_t data_len, const char* old, const char* repl, const char* key,
                             const char* what, char* err, size_t err_size)
{
  /* 1. the entry: [be32 L][be32 ulen][zlib stream ...] holding `old` exactly once */
  size_t entry = 0, hits = 0;
  uint8_t* text = NULL;
  size_t text_len = 0;
  const size_t ol = strlen(old);
  for (size_t i = 8; i + 16 < data_len; ++i)
  {
    if (data[i] != 0x78 || (data[i + 1] != 0x9c && data[i + 1] != 0xda && data[i + 1] != 0x01 && data[i + 1] != 0x5e))
      continue;
    const uint32_t L = rd_be32(data + i - 8), ulen = rd_be32(data + i - 4);
    if (!(ulen > 1000 && ulen < 400000 && L > 100 && L < 400000) || i - 4 + L > data_len)
      continue;
    uint8_t* buf = malloc(ulen + 1);
    if (!buf)
      continue;
    mz_ulong dl = ulen;
    if (mz_uncompress(buf, &dl, data + i, L - 4) == MZ_OK && dl == ulen)
    {
      buf[ulen] = 0;
      const uint8_t* a = find_mem(buf, ulen, (const uint8_t*)old, ol, NULL);
      if (a)
      {
        ++hits;
        entry = i - 8;
        free(text);
        text = buf;
        text_len = ulen;
        if (find_mem(buf, ulen, (const uint8_t*)old, ol, a + 1))
          hits = 99;
        continue;
      }
    }
    free(buf);
  }
  if (hits != 1)
  {
    free(text);
    set_err(err, err_size, "%s: edit point found %zu times (expected once); not the stock Pedalboard 2.7 Evil", what, hits);
    return false;
  }

  /* 2. new text */
  const size_t rl = strlen(repl);
  const uint8_t* at = find_mem(text, text_len, (const uint8_t*)old, ol, NULL);
  const size_t pre = (size_t)(at - text);
  char* nt = malloc(text_len + rl + 1);
  if (!nt)
  {
    free(text);
    set_err(err, err_size, "out of memory");
    return false;
  }
  memcpy(nt, text, pre);
  memcpy(nt + pre, repl, rl);
  memcpy(nt + pre + rl, text + pre + ol, text_len - pre - ol);
  size_t nlen = text_len - ol + rl;
  free(text);

  /* 3. compress into the old slot (comment and blank lines dropped if it does not fit) */
  const uint32_t L = rd_be32(data + entry);
  const size_t room = L - 4;
  mz_ulong zl = mz_compressBound((mz_ulong)nlen);
  uint8_t* z = malloc(zl);
  bool fits = false;
  if (z && mz_compress2(z, &zl, (const uint8_t*)nt, (mz_ulong)nlen, 9) == MZ_OK && zl <= room)
    fits = true;
  if (!fits && z)
  {
    char* mn = malloc(nlen + 1);
    if (mn)
    {
      const size_t mlen = minify(nt, nlen, mn);
      zl = mz_compressBound((mz_ulong)mlen);
      if (mz_compress2(z, &zl, (const uint8_t*)mn, (mz_ulong)mlen, 9) == MZ_OK && zl <= room)
      {
        fits = true;
        nlen = mlen;
      }
      free(mn);
    }
  }
  if (!fits)
  {
    set_err(err, err_size, "%s: edited source does not fit its slot (%lu > %zu bytes)", what, (unsigned long)zl, room);
    free(nt);
    free(z);
    return false;
  }
  uint8_t* body = data + entry + 4;
  memset(body, 0, L);
  wr_be32(body, (uint32_t)nlen);
  memcpy(body + 4, z, zl);
  free(nt);
  free(z);

  /* 4. break the unit key so the lookup misses and the (now edited) source is used */
  return break_key(data, data_len, key, err, err_size);
}

bool nam_qml_override_blockdetails(uint8_t* data, size_t data_len, const char* page_url, char* err, size_t err_size)
{
  if (strlen(page_url) > 120)
  {
    set_err(err, err_size, "QML page url too long");
    return false;
  }
  char repl[512];
  snprintf(repl, sizeof(repl),
           "source: blckEdt.visible ? (curModuleUIFile.translator.string.indexOf(\"MorningGlory\") >= 0 ? \"%s\" : "
           "curModuleUIFile.translator.string + \".qml\") : \"\"",
           page_url);
  if (!patch_zlib_entry(data, data_len, k_old, repl, k_key, "BlockDetails.qml", err, err_size))
    return false;
  /* the picture on the left: for pedals without a picture of their own (the stock page falls back to Other.png and they
   * all end up here), a Loader of the drawn pedal; the art shows the stock fallback itself for everything else */
  static const char k_img[] =
      "Image {source: \"ModuleEditors/img/Other.png\"; visible: preview.status === Image.Error && !parent.isAmp() && "
      "!parent.isCab()}";
  static const char k_art[] =
      "Loader {source: \"file:///usr/Evil/Resources/pedal_art.qml\"; visible: preview.status === Image.Error && "
      "!parent.isAmp() && !parent.isCab()}";
  return patch_zlib_entry(data, data_len, k_img, k_art, k_key, "BlockDetails.qml (picture)", err, err_size);
}

bool nam_qml_override_stomp(uint8_t* data, size_t data_len, char* err, size_t err_size)
{
  static const char key[] = "/Groups/SignalChainModuleStomps/ActiveStomp.qml";
  /* the Reverb's stock stomp picture is recoloured blue */
  static const char hue_old[] = "hue: hslColor[0] ";
  static const char hue_new[] = "hue: signalChainModule.moduleName.indexOf(\"Reverb\") == 0 ? 0.58 : hslColor[0] ";
  /* the overlay (knobs, model / algorithm caption) goes right after the stock LED picture */
  static const char led_old[] =
      "source: signalChainModule.active ? \"../../img/FXModule_Active_5_StompSwitchLED.png\" : "
      "\"../../img/FXModule_Bypassed_3_StompSwitchLED.png\"";
  static const char led_new[] =
      "source: signalChainModule.active ? \"../../img/FXModule_Active_5_StompSwitchLED.png\" : "
      "\"../../img/FXModule_Bypassed_3_StompSwitchLED.png\"\n\t\t}\n\t\tLoader {\n\t\t\tanchors.fill: parent\n\t\t\tsource: "
      "signalChainModule.moduleName.indexOf(\"NAM\") == 0 || signalChainModule.moduleName.indexOf(\"Reverb\") == 0 ? "
      "\"file:///usr/Evil/Resources/rig_block.qml\" : \"\"\n\t\t\tonLoaded: item.m = signalChainModule";
  return patch_zlib_entry(data, data_len, hue_old, hue_new, key, "ActiveStomp.qml (hue)", err, err_size)
         && patch_zlib_entry(data, data_len, led_old, led_new, key, "ActiveStomp.qml (overlay)", err, err_size);
}

bool nam_qml_override_encoder_field(uint8_t* data, size_t data_len, char* err, size_t err_size)
{
  static const char old[] =
      "property int numParams: Evil.getProperty(\"/Engine/RedirCtrl/NumEditParams\").translator.unnormalized";
  static const char repl[] =
      "property int numParams: Evil.getProperty(\"/Engine/RedirCtrl/NumEditParams\").translator.unnormalized - "
      "(function(){var t=Evil.getProperty(\"/Engine/Patch/Chain/ModuleType\" + "
      "Evil.getProperty(\"/Engine/RedirCtrl/CurModule\").translator.index);var s=t && t.translator ? "
      "t.translator.string : \"\";return s.indexOf(\"NAM\") == 0 ? 1 : (s.indexOf(\"Reverb\") == 0 ? 2 : 0)})()";
  return patch_zlib_entry(data, data_len, old, repl, "/Pages/EncoderField.qml", "EncoderField.qml", err, err_size);
}

bool nam_qml_override_encoder_tile(uint8_t* data, size_t data_len, const char* qml, size_t qml_len, char* err,
                                   size_t err_size)
{
  /* plain entry: [be32 L][source, L bytes]; identified by its start and two lines only this file has */
  static const char k_head[] = "import QtQuick 2.2\nimport QtQuick.Controls 1.2\nimport StateMap 1.0\n";
  static const char k_a[] = "ParamNameHi%1";
  static const char k_b[] = "frame.showPedalNoLock";
  const size_t hl = sizeof(k_head) - 1;
  size_t entry = 0, hits = 0;
  uint32_t L = 0;
  for (const uint8_t* p = find_mem(data, data_len, (const uint8_t*)k_head, hl, NULL); p;
       p = find_mem(data, data_len, (const uint8_t*)k_head, hl, p + 1))
  {
    const size_t i = (size_t)(p - data);
    if (i < 4)
      continue;
    const uint32_t l = rd_be32(data + i - 4);
    if (l < 500 || l > 20000 || i + l > data_len)
      continue;
    if (find_mem(p, l, (const uint8_t*)k_a, sizeof(k_a) - 1, NULL) && find_mem(p, l, (const uint8_t*)k_b, sizeof(k_b) - 1, NULL))
    {
      ++hits;
      entry = i - 4;
      L = l;
    }
  }
  if (hits != 1)
  {
    set_err(err, err_size, "EncoderParamInfo.qml found %zu times (expected once); not the stock Pedalboard 2.7 Evil", hits);
    return false;
  }
  if (qml_len > L)
  {
    set_err(err, err_size, "edited EncoderParamInfo.qml does not fit its slot (%zu > %u bytes)", qml_len, (unsigned)L);
    return false;
  }
  memset(data + entry + 4, ' ', L);
  memcpy(data + entry + 4, qml, qml_len);
  return break_key(data, data_len, "/Groups/EncoderParamInfo.qml", err, err_size);
}

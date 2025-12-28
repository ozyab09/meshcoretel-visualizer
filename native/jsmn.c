/*
 * jsmn.c
 * Minimalistic JSON parser in C.
 * Source: https://github.com/zserge/jsmn (MIT license)
 */
#include "jsmn.h"

static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens,
                                  size_t num_tokens) {
  if (parser->toknext >= num_tokens) {
    return NULL;
  }
  jsmntok_t *tok = &tokens[parser->toknext++];
  tok->start = -1;
  tok->end = -1;
  tok->size = 0;
  tok->parent = -1;
  tok->type = JSMN_UNDEFINED;
  return tok;
}

static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type, int start,
                            int end) {
  token->type = type;
  token->start = start;
  token->end = end;
  token->size = 0;
}

static int jsmn_parse_primitive(jsmn_parser *parser, const char *js, size_t len,
                                jsmntok_t *tokens, size_t num_tokens) {
  int start = parser->pos;
  for (; parser->pos < len; parser->pos++) {
    switch (js[parser->pos]) {
      case '\t': case '\r': case '\n': case ' ': case ',': case ']': case '}':
        goto found;
      default:
        break;
    }
    if (js[parser->pos] < 32 || js[parser->pos] >= 127) {
      parser->pos = start;
      return -1;
    }
  }
found:
  if (tokens == NULL) {
    parser->pos--;
    return 0;
  }
  jsmntok_t *token = jsmn_alloc_token(parser, tokens, num_tokens);
  if (token == NULL) {
    parser->pos = start;
    return -1;
  }
  jsmn_fill_token(token, JSMN_PRIMITIVE, start, parser->pos);
  token->parent = parser->toksuper;
  parser->pos--;
  return 0;
}

static int jsmn_parse_string(jsmn_parser *parser, const char *js, size_t len,
                             jsmntok_t *tokens, size_t num_tokens) {
  int start = parser->pos;
  parser->pos++;

  for (; parser->pos < len; parser->pos++) {
    char c = js[parser->pos];
    if (c == '"') {
      if (tokens == NULL) {
        return 0;
      }
      jsmntok_t *token = jsmn_alloc_token(parser, tokens, num_tokens);
      if (token == NULL) {
        parser->pos = start;
        return -1;
      }
      jsmn_fill_token(token, JSMN_STRING, start + 1, parser->pos);
      token->parent = parser->toksuper;
      return 0;
    }
    if (c == '\\' && parser->pos + 1 < len) {
      parser->pos++;
      switch (js[parser->pos]) {
        case '"': case '/': case '\\': case 'b': case 'f': case 'r':
        case 'n': case 't':
          break;
        case 'u':
          parser->pos += 4;
          break;
        default:
          parser->pos = start;
          return -1;
      }
    }
  }
  parser->pos = start;
  return -1;
}

void jsmn_init(jsmn_parser *parser) {
  parser->pos = 0;
  parser->toknext = 0;
  parser->toksuper = -1;
}

int jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
               jsmntok_t *tokens, unsigned int num_tokens) {
  for (; parser->pos < len; parser->pos++) {
    char c = js[parser->pos];
    switch (c) {
      case '{': case '[': {
        jsmntok_t *token = NULL;
        if (tokens != NULL) {
          token = jsmn_alloc_token(parser, tokens, num_tokens);
          if (token == NULL) {
            return -1;
          }
          token->type = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY);
          token->start = parser->pos;
          token->parent = parser->toksuper;
          if (parser->toksuper != -1) {
            tokens[parser->toksuper].size++;
          }
        }
        parser->toksuper = parser->toknext - 1;
        break;
      }
      case '}': case ']': {
        jsmntype_t type = (c == '}' ? JSMN_OBJECT : JSMN_ARRAY);
        if (tokens == NULL) {
          break;
        }
        int i = parser->toknext - 1;
        for (; i >= 0; i--) {
          if (tokens[i].start != -1 && tokens[i].end == -1) {
            if (tokens[i].type != type) {
              return -1;
            }
            tokens[i].end = parser->pos + 1;
            parser->toksuper = tokens[i].parent;
            break;
          }
        }
        if (i == -1) {
          return -1;
        }
        break;
      }
      case '"': {
        int r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
        if (r < 0) {
          return r;
        }
        if (parser->toksuper != -1 && tokens != NULL) {
          tokens[parser->toksuper].size++;
        }
        break;
      }
      case '\t': case '\r': case '\n': case ' ': case ':': case ',':
        break;
      default: {
        int r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
        if (r < 0) {
          return r;
        }
        if (parser->toksuper != -1 && tokens != NULL) {
          tokens[parser->toksuper].size++;
        }
        break;
      }
    }
  }
  for (int i = parser->toknext - 1; i >= 0; i--) {
    if (tokens[i].start != -1 && tokens[i].end == -1) {
      return -1;
    }
  }
  return (int)parser->toknext;
}

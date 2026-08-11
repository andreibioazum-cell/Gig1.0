#!/usr/bin/env python3
"""DimScript — минимальный язык для игры, компиляция в C.

Типы: num, str, col. Блоки закрываются словом `end`, вызовы — без скобок.

    str TEX = "player.png"

    object Player              // структура с полями
        num x = 0
        col color = 0xFF8844
    end

    Player player = new Player()
    player.x = 100

    function move_player num dx, num dy
        player.x = player.x + dx
        if player.x > 10
            return
        end
        loop player.x < 100
            player.x = player.x + 1
        end
    end

    move_player 10, 0          // вызов функции
    circle player.x, player.y, 15, player.color   // встроенная функция
"""

import os
import re
import sys

# Типы языка и их C-представление.
TYPES = {'num': 'double', 'str': 'const char *', 'col': 'uint32_t'}

# Встроенные функции (имя в скрипте = имя в C).
BUILTINS = frozenset({
    # отрисовка
    'rect', 'roundrect', 'circle', 'ring', 'line', 'tex', 'text',
    'text_scaled', 'text_ink_width', 'text_ink_height', 'png_load',
    # математика
    'sqrt', 'sin', 'cos', 'atan2', 'floor', 'rand',
    # звёзды фона лобби
    'init_stars', 'update_stars', 'draw_stars',
    # сеть (онлайн)
    'net_connect', 'net_close', 'net_create', 'net_join',
    'net_send_pos', 'net_send_bullet', 'net_send_hit', 'net_poll',
})

# Глобальные переменные хоста (read-only, кроме joy и net_*).
ENGINE_VARS = {
    'screen_w': 'num', 'screen_h': 'num', 'dt': 'num', 'joy': 'joy',
    # networking globals
    'net_state': 'num', 'net_am_host': 'num',
    'net_peer_x': 'num', 'net_peer_y': 'num', 'net_peer_angle': 'num',
    'net_peer_bx': 'num', 'net_peer_by': 'num',
    'net_peer_bdx': 'num', 'net_peer_bdy': 'num',
    'net_peer_fire': 'num', 'net_peer_hit': 'num',
}

_NAME = r'[A-Za-z_]\w*'
_DECL_RE = re.compile(r'^(' + _NAME + r')\s+(' + _NAME + r')\s*(?:=\s*(.*))?$')
_FUNC_RE = re.compile(r'^function\s+(' + _NAME + r')(?:\s+(.*))?$')
_NUM_RE = re.compile(r'^(?:[-+]?\d+(?:\.\d+)?|0[xX][0-9a-fA-F]+)$')
_CALL_RE = re.compile(r'^(' + _NAME + r')(?:\s+(.*))?$')
_LHS_RE = re.compile(r'^(' + _NAME + r')(?:\.(' + _NAME + r'))?$')


def strip_comment(line):
    """Убирает //-комментарий, не трогая строковые литералы."""
    out = []
    i = 0
    in_str = False
    while i < len(line):
        c = line[i]
        if in_str:
            out.append(c)
            if c == '\\' and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            out.append(c)
        elif c == '/' and i + 1 < len(line) and line[i + 1] == '/':
            break
        else:
            out.append(c)
        i += 1
    return ''.join(out)


def scan(text):
    """Для каждого символа: вложенность скобок и флаг «внутри строки»."""
    n = len(text)
    depth = [0] * n
    quoted = [False] * n
    in_str = False
    escaped = False
    level = 0
    for i, c in enumerate(text):
        quoted[i] = in_str
        if escaped:
            escaped = False
            continue
        if in_str:
            if c == '\\':
                escaped = True
            elif c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
        elif c == '(':
            level += 1
        elif c == ')':
            level = max(0, level - 1)
        depth[i] = level
    return depth, quoted


def split_top(text, sep):
    """Делит выражение по разделителю на верхнем уровне (вне строк и скобок)."""
    depth, quoted = scan(text)
    parts = []
    start = 0
    for i, c in enumerate(text):
        if depth[i] == 0 and not quoted[i] and c == sep:
            parts.append(text[start:i].strip())
            start = i + 1
    parts.append(text[start:].strip())
    return parts


def find_assign(line):
    """Индекс простого `=` (не ==, !=, <=, >=) вне строк и скобок, иначе -1."""
    depth, quoted = scan(line)
    for i, c in enumerate(line):
        if quoted[i] or depth[i]:
            continue
        if c == '=' and (i == 0 or line[i - 1] not in '<>!') and (
                i + 1 >= len(line) or line[i + 1] != '='):
            return i
    return -1


def used_outside_strings(text, name):
    """Встречается ли идентификатор в тексте вне строковых литералов."""
    pat = re.compile(r'\b' + re.escape(name) + r'\b')
    _, quoted = scan(text)
    for m in pat.finditer(text):
        if not quoted[m.start()]:
            return True
    return False


class DimScriptCompiler:
    def __init__(self):
        self.objects = {}        # имя -> поля {имя: (тип, значение)}
        self.vars = {}           # глобальные переменные {имя: (тип, значение)}
        self.functions = {}      # имя -> (параметры [(тип, имя)], тело [строки])
        self.top = []            # исполняемые строки верхнего уровня
        self.lines = []          # все строки исходника после include-склейки
        self.loaded_sources = [] # прочитанные файлы
        self.errors = 0
        # состояние генерации
        self.output = []
        self.indent = 0
        self.scope = {}          # локальные переменные и параметры: имя -> тип
        self.blocks = []         # стек открытых if/loop

    # ---------- чтение и разбор ----------

    def _error(self, msg):
        self.errors += 1
        print(f"DimScript error: {msg}", file=sys.stderr)

    def _load(self, paths):
        for path in paths:
            try:
                with open(path, 'r', encoding='utf-8-sig') as f:
                    for raw in f:
                        line = strip_comment(raw).strip()
                        if line:
                            self.lines.append(line)
                self.loaded_sources.append(os.path.abspath(path))
            except OSError as e:
                self._error(f"cannot read '{path}': {e}")
                return False
        return True

    def parse(self):
        i = 0
        while i < len(self.lines):
            line = self.lines[i]
            if line == 'end':
                self._error("unexpected 'end' at top level")
                i += 1
            elif line.startswith('object '):
                i = self._parse_object(i)      # уже индекс следующей строки
            elif line.startswith('function '):
                i = self._parse_function(i)
            elif self._decl(line):
                self._parse_global(line)
                i += 1
            else:
                self.top.append(line)
                i += 1
        return self.errors == 0

    def _decl(self, line):
        m = _DECL_RE.match(line)
        if not m:
            return None
        t, n, v = m.group(1), m.group(2), m.group(3)
        if t not in TYPES and t not in self.objects:
            return None
        return t, n, v.strip() if v else None

    def _parse_object(self, i):
        m = re.match(r'^object\s+(' + _NAME + r')\s*$', self.lines[i])
        if not m:
            self._error(f"invalid object declaration: {self.lines[i]}")
            return i + 1
        name = m.group(1)
        if name in self.objects:
            self._error(f"duplicate object '{name}'")
            return i + 1
        fields = {}
        j = i + 1
        while j < len(self.lines):
            line = self.lines[j]
            if line == 'end':
                self.objects[name] = fields
                return j + 1
            d = self._decl(line)
            if not d or d[0] not in TYPES:
                self._error(f"object '{name}': expected 'type name = value', got: {line}")
            else:
                t, n, v = d
                if n in fields:
                    self._error(f"duplicate field '{name}.{n}'")
                else:
                    fields[n] = (t, v)
            j += 1
        self._error(f"object '{name}' has no closing 'end'")
        return j

    def _parse_function(self, i):
        m = _FUNC_RE.match(self.lines[i])
        name = m.group(1)
        params = self._parse_params(m.group(2) or '')
        body, j = self._collect_block(i + 1, f"function '{name}'")
        if name in self.functions:
            self._error(f"duplicate function '{name}'")
        else:
            self.functions[name] = (params, body)
        return j

    def _parse_params(self, text):
        params = []
        if not text.strip():
            return params
        for part in split_top(text, ','):
            w = part.split()
            if len(w) != 2 or w[0] not in TYPES:
                self._error(f"invalid parameter '{part}'; expected 'type name'")
                continue
            params.append((w[0], w[1]))
        return params

    def _collect_block(self, i, what):
        """Собирает тело блока (function/object) до парного `end`."""
        depth = 0
        body = []
        while i < len(self.lines):
            line = self.lines[i]
            if line == 'end':
                if depth == 0:
                    return body, i + 1
                depth -= 1
            elif line.startswith('if ') or line.startswith('loop '):
                depth += 1
            elif line == 'else' or line.startswith('else if '):
                if depth == 0:
                    self._error(f"{what}: 'else' without 'if'")
                    return body, i + 1
            elif line.startswith('object ') or line.startswith('function '):
                self._error(f"{what}: nested 'object'/'function' is not allowed")
                body.append(line)
                i += 1
                continue
            body.append(line)
            i += 1
        self._error(f"{what} has no closing 'end'")
        return body, i

    def _parse_global(self, line):
        t, n, v = self._decl(line)
        if n in self.vars:
            self._error(f"duplicate variable '{n}'")
            return
        if t in self.objects:
            if not v or not re.match(r'^new\s+' + re.escape(t) + r'\s*\(\)?\s*$', v):
                self._error(f"'{n}': object variable must be 'new {t}()'")
                return
        self.vars[n] = (t, v)

    # ---------- типы и выражения ----------

    def c_type(self, t):
        if t in TYPES:
            return TYPES[t]
        if t in self.objects:
            return t + ' *'
        return t

    def default_value(self, t):
        return 'NULL' if t == 'str' else '0'

    def static_expr(self, v):
        return bool(_NUM_RE.match(v)) or (
            len(v) >= 2 and v[0] == '"' and v[-1] == '"')

    def expr_type(self, expr):
        expr = expr.strip()
        if expr.startswith('"') and expr.endswith('"'):
            return 'str'
        m = re.match(r'^(' + _NAME + r')\.(' + _NAME + r')$', expr)
        if m and m.group(1) in self.vars:
            ot = self.vars[m.group(1)][0]
            fields = self.objects.get(ot)
            if fields and m.group(2) in fields:
                return fields[m.group(2)][0]
        if expr in self.scope:
            return self.scope[expr]
        if expr in self.vars:
            return self.vars[expr][0]
        return ENGINE_VARS.get(expr, 'num')

    def expr(self, e):
        """Переводит выражение в C."""
        e = e.strip()
        parts = split_top(e, '+')
        if len(parts) > 1 and all(parts) and any(
                self.expr_type(p) == 'str' for p in parts):
            out = self.as_str(parts[0])
            for p in parts[1:]:
                out = f'ds_concat({out}, {self.as_str(p)})'
            return out
        return self._fields(e)

    def _fields(self, e):
        """Превращает obj.field в obj->field вне строковых литералов."""
        names = [n for n in self.vars if self.vars[n][0] in self.objects]
        for n in sorted(names, key=len, reverse=True):
            pat = re.compile(r'\b' + re.escape(n) + r'\.(' + _NAME + r')')
            repl = n + r'->\1'
            _, quoted = scan(e)
            if not any(quoted):
                e = pat.sub(repl, e)
                continue
            out = []
            start = 0
            for m in pat.finditer(e):
                if quoted[m.start()]:
                    continue
                out.append(e[start:m.start()])
                out.append(m.expand(repl))
                start = m.end()
            out.append(e[start:])
            e = ''.join(out)
        return e

    def as_str(self, e):
        if self.expr_type(e) == 'str':
            return self.expr(e)
        return f'ds_num_to_string((double)({self.expr(e)}))'

    # ---------- генерация C ----------

    def _out(self, s):
        self.output.append('    ' * self.indent + s)

    def _emit(self, s):
        self.output.append(s)

    def _emit_line(self, line):
        if line == 'end':
            if not self.blocks:
                self._error("unexpected 'end'")
                return
            self.blocks.pop()
            self.indent -= 1
            self._out('}')
            return
        if line.startswith('if '):
            self._open_block(f'if ({self.expr(line[3:])})')
            return
        if line.startswith('loop '):
            self._open_block(f'while ({self.expr(line[5:])})')
            return
        if line == 'else' or line.startswith('else if '):
            if not self.blocks:
                self._error("'else' without 'if'")
                return
            header = 'else' if line == 'else' else f'else if ({self.expr(line[8:])})'
            self.indent -= 1
            self._out(f'}} {header} {{')
            self.indent += 1
            return
        if line == 'return':
            self._out('return;')
            return
        d = self._decl(line)
        if d and d[0] in TYPES:
            t, n, v = d
            if n in self.scope:
                self._error(f"duplicate variable '{n}'")
                return
            self.scope[n] = t
            init = f'= {self.expr(v)}' if v else f'= {self.default_value(t)}'
            self._out(f'{self.c_type(t)} {n} {init};')
            return
        self._emit_statement(line)

    def _open_block(self, header):
        self.blocks.append(header)
        self._out(header + ' {')
        self.indent += 1

    def _emit_statement(self, line):
        i = find_assign(line)
        if i >= 0:
            self._emit_assign(line[:i].strip(), line[i + 1:].strip())
            return
        m = _CALL_RE.match(line)
        if not m:
            self._error(f"invalid statement: {line}")
            return
        name, rest = m.group(1), m.group(2) or ''
        args = split_top(rest, ',') if rest else []
        if name in self.functions:
            if len(args) != len(self.functions[name][0]):
                self._error(f"function '{name}' expects "
                            f"{len(self.functions[name][0])} argument(s), got {len(args)}")
                return
            fn = f'ds_fn_{name}'
        elif name in BUILTINS:
            fn = name
        elif name in ENGINE_VARS or name in self.vars or name in self.scope:
            self._error(f"'{name}' is a variable, not a function")
            return
        else:
            self._error(f"unknown function '{name}'")
            return
        args_c = ', '.join(self.expr(a) for a in args)
        self._out(f'{fn}({args_c});')

    def _emit_assign(self, lhs, rhs):
        m = _LHS_RE.match(lhs)
        if not m:
            self._error(f"invalid assignment target: {lhs}")
            return
        name, field = m.group(1), m.group(2)
        if rhs.startswith('new '):
            self._error(f"'{lhs} = {rhs}': use 'Type name = new Type()'")
            return
        if field:
            t = self.vars.get(name, ('', None))[0]
            if t not in self.objects and ENGINE_VARS.get(name) != 'joy':
                self._error(f"unknown object '{name}'")
                return
        elif name not in self.scope and name not in self.vars and name not in ENGINE_VARS:
            self._error(f"unknown variable '{name}'")
        if field and self.vars.get(name, ('', None))[0] in self.objects:
            lhs = self._fields(lhs)
        self._out(f'{lhs} = {self.expr(rhs)};')

    def generate(self):
        self.output = []
        self.indent = 0
        self._emit('#include "runtime.h"')
        self._emit('#include <math.h>')
        self._emit('')
        # структуры объектов
        for name in self.objects:
            self._emit(f'typedef struct {name} {name};')
        self._emit('')
        for name, fields in self.objects.items():
            self._emit(f'struct {name} {{')
            for f, (t, _v) in fields.items():
                self._emit(f'    {self.c_type(t)} {f};')
            self._emit('};')
        self._emit('')
        for name in self.objects:
            self._emit(f'static {name} *ds_new_{name}(void);')
            self._emit(f'static void ds_free_{name}({name} *self);')
        self._emit('')
        # глобальные переменные
        init_lines = []
        for n, (t, v) in self.vars.items():
            if t in self.objects:
                self._emit(f'{t} *{n} = NULL;')
                init_lines.append(n)
            elif v and self.static_expr(v):
                self._emit(f'{self.c_type(t)} {n} = {self.expr(v)};')
            else:
                self._emit(f'{self.c_type(t)} {n} = {self.default_value(t)};')
                if v:
                    init_lines.append(n)
        if self.vars:
            self._emit('')
        # прототипы функций
        for n, (params, _b) in self.functions.items():
            self._emit(f'static void ds_fn_{n}({self._params_c(params)});')
        if self.functions:
            self._emit('')
        # конструкторы и деструкторы
        for name, fields in self.objects.items():
            self._emit(f'static {name} *ds_new_{name}(void) {{')
            self._emit(f'    {name} *self = ({name} *)calloc(1, sizeof(*self));')
            self._emit(f'    if (!self) {{ ds_runtime_error("out of memory: {name}"); return NULL; }}')
            for f, (t, v) in fields.items():
                if v:
                    self._emit(f'    self->{f} = {self.expr(v)};')
            self._emit('    return self;')
            self._emit('}')
            self._emit(f'static void ds_free_{name}({name} *self) {{ free(self); }}')
            self._emit('')
        # функции
        for n, (params, body) in self.functions.items():
            self._emit(f'static void ds_fn_{n}({self._params_c(params)}) {{')
            self.indent = 1
            self.scope = {pn: pt for pt, pn in params}
            self.blocks = []
            body_text = '\n'.join(body)
            for _pt, pn in params:
                if not used_outside_strings(body_text, pn):
                    self._out(f'(void){pn};')
            for line in body:
                self._emit_line(line)
            if self.blocks:
                self._error(f"function '{n}': missing 'end'")
                self.blocks = []
            self._emit('}')
            self._emit('')
        # запуск верхнего уровня (объекты и команды в порядке исходника)
        self._emit('static int ds_main(void) {')
        self.indent = 1
        self.scope = {}
        self.blocks = []
        for n in init_lines:
            t = self.vars[n][0]
            if t in self.objects:
                self._out(f'{n} = ds_new_{t}();')
            else:
                self._out(f'{n} = {self.expr(self.vars[n][1])};')
        for line in self.top:
            self._emit_line(line)
        if self.blocks:
            self._error("top level: missing 'end'")
            self.blocks = []
        self._emit('    return 0;')
        self._emit('}')
        self._emit('')
        # хуки хоста
        self._emit('void reset(void) {')
        self.indent = 1
        for n, (t, v) in self.vars.items():
            if t in self.objects:
                self._out(f'if ({n}) ds_free_{t}({n});')
                self._out(f'{n} = NULL;')
            elif v and self.static_expr(v):
                self._out(f'{n} = {self.expr(v)};')
            else:
                self._out(f'{n} = {self.default_value(t)};')
        self._emit('}')
        self._emit('')
        self._emit('void init(AAssetManager *assets) {')
        self.indent = 1
        self._out('ds_set_asset_manager(assets);')
        self._out('ds_main();')
        if 'init' in self.functions:
            self._out('ds_fn_init();')
        self._emit('}')
        self._emit('')
        self._emit('void update(void) {')
        self.indent = 1
        if 'update' in self.functions:
            self._out('ds_fn_update();')
        self._emit('}')
        self._emit('')
        self._emit('void draw(Buffer *buffer) {')
        self.indent = 1
        self._out('(void)buffer;')
        if 'draw' in self.functions:
            self._out('ds_fn_draw();')
        self._emit('}')
        self._emit('')
        self._emit('void touch(float x, float y, int action, int pointer_id) {')
        self.indent = 1
        if 'touch' in self.functions:
            args = []
            for i, (pt, _pn) in enumerate(self.functions['touch'][0]):
                if i >= 4:
                    break
                if pt == 'str':
                    self._error("'touch' parameters cannot be 'str'")
                    break
                args.append(f'({self.c_type(pt)}){("x", "y", "action", "pointer_id")[i]}')
            self._out(f'ds_fn_touch({", ".join(args)});')
        else:
            self._out('(void)x; (void)y; (void)action; (void)pointer_id;')
        self._emit('}')

    def _params_c(self, params):
        if not params:
            return 'void'
        return ', '.join(f'{self.c_type(t)} {n}' for t, n in params)

    def compile(self, sources, output):
        if not self._load(sources):
            return False
        if not self.parse():
            return False
        self.generate()
        if self.errors:
            return False
        with open(output, 'w', encoding='utf-8') as f:
            f.write('\n'.join(self.output) + '\n')
        return True


def main():
    output = 'game/game.c'
    sources = []
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] in ('-o', '--output') and i + 1 < len(args):
            output = args[i + 1]
            i += 2
        else:
            sources.append(args[i])
            i += 1
    if not sources:
        print("Usage: python ds_compiler.py file.ds [-o output.c]", file=sys.stderr)
        sys.exit(2)
    ok = DimScriptCompiler().compile(sources, output)
    print(f"{output}: {'OK' if ok else 'FAILED'}")
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()

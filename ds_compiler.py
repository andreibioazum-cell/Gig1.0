import re, sys
TYPES={'num':'double','str':'const char*','col':'uint32_t','arr':'DSArray*'}
BUILTINS=frozenset({
    'rect','roundrect','circle','ring','line','tex','text','text_scaled','text_ink_width','text_ink_height','text_ink_top','png_load',
    'sqrt','sin','cos','atan2','floor','rand',
    'net_connect','net_disconnect','net_publish','net_publish_punch','net_publish_gift','net_set_class','net_status','net_slot',
    'net_player_online','net_player_x','net_player_y','net_player_angle','net_player_hp','net_player_alive','net_player_nick',
    'net_player_punch_x','net_player_punch_y','net_player_punch_dx','net_player_punch_dy','net_player_punch',
    'net_player_class','net_player_freeze',
    'net_player_gift','net_player_gift_x','net_player_gift_y','net_player_gift_dx','net_player_gift_dy',
    'net_event','net_event_fetch','net_event_set',
    'net_chat_send','net_chat_trim','net_chat_count','net_chat_text','net_chat_uid',
    'net_autologin','net_set_nick','net_login_status','net_login_nick','net_auth','net_logout',
    'net_leaderboard_fetch','net_leaderboard_status','net_leaderboard_count','net_leaderboard_nick','net_leaderboard_cups',
    'net_load_cups','net_load_candies','net_load_class','net_load_azum','net_load_santa','net_load_level','net_load_levels_unlocked','net_save_progress',
    'keyboard_show','keyboard_hide','keyboard_get_text','keyboard_get_raw','keyboard_clear','keyboard_enter_pressed','keyboard_type','keyboard_visible',
    'str_len','str_eq','str_ieq','str_has',
    'ds_log','console_count','console_line','console_type','console_clear',
    'arr_new','arr_push','arr_get','arr_set','arr_len','arr_clear',
    'clamp','lerp','dist'
})
ENGINE_VARS={'screen_w':'num','screen_h':'num','dt':'num','joy':'joy'}
STR_BUILTINS=frozenset({'console_line','keyboard_get_text','keyboard_get_raw','net_chat_text','net_chat_uid','net_login_nick','net_player_nick','net_leaderboard_nick'})
_NAME=r'[A-Za-z_]\w*'
_FUNC_RE=re.compile(r'^function\s+('+_NAME+r')(?:\s+(.*))?$')
_NUM_RE=re.compile(r'^(?:[-+]?\d+(?:\.\d+)?|0[xX][0-9a-fA-F]+)$')
_CALL_RE=re.compile(r'^('+_NAME+r')(?:\s+(.*))?$')
_LHS_RE=re.compile(r'^('+_NAME+r')(?:\.('+_NAME+r'))?$')
def strip_comment(line):
    out=[]; i=0; in_str=False
    while i < len(line):
        c=line[i]
        if in_str:
            out.append(c)
            if c=='\\' and i+1 < len(line): out.append(line[i+1]); i+=2; continue
            if c=='"': in_str=False
        elif c=='"': in_str=True; out.append(c)
        elif c=='/' and i+1 < len(line) and line[i+1]=='/': break
        else: out.append(c)
        i+=1
    return ''.join(out)
def scan(text):
    n=len(text); depth=[0]*n; quoted=[False]*n; in_str=False; esc=False; lvl=0
    for i,c in enumerate(text):
        quoted[i]=in_str
        if esc: esc=False; continue
        if in_str:
            if c=='\\': esc=True
            elif c=='"': in_str=False
            continue
        if c=='"': in_str=True
        elif c=='(': lvl+=1
        elif c==')': lvl=max(0,lvl-1)
        depth[i]=lvl
    return depth,quoted
def split_top(text,sep):
    depth,quoted=scan(text); parts=[]; start=0
    for i,c in enumerate(text):
        if depth[i]==0 and not quoted[i] and c==sep:
            parts.append(text[start:i].strip()); start=i+1
    parts.append(text[start:].strip()); return parts
def find_assign(line):
    depth,quoted=scan(line)
    for i,c in enumerate(line):
        if quoted[i] or depth[i]: continue
        if c=='=' and (i==0 or line[i-1] not in '<>!') and (i+1>=len(line) or line[i+1]!='='): return i
    return -1
def used_outside_strings(text,name):
    pat=re.compile(r'\b'+re.escape(name)+r'\b'); _,quoted=scan(text)
    return any(not quoted[m.start()] for m in pat.finditer(text))
def sub_unquoted(e,pat,repl):
    """Replace pat with repl outside string literals."""
    _,quoted=scan(e)
    if not any(quoted): return pat.sub(repl,e)
    out=[]; start=0
    for m in pat.finditer(e):
        if quoted[m.start()]: continue
        out.append(e[start:m.start()]); out.append(m.expand(repl)); start=m.end()
    out.append(e[start:]); return ''.join(out)
def strip_kw(cond,kw):
    """Drop an optional trailing 'then'/'do' and/or ':' from a block header."""
    for suf in (' '+kw,' '+kw+':'):
        if cond.endswith(suf): cond=cond[:-len(suf)].strip(); break
    return cond[:-1].strip() if cond.endswith(':') else cond
class DimScriptCompiler:
    def __init__(self):
        self.objects={}; self.vars={}; self.functions={}; self.func_ret={}; self.top=[]; self.lines=[]; self.errors=0; self.output=[]; self.indent=0; self.scope={}; self.blocks=[]
    def _error(self,msg): self.errors+=1; print(f"DimScript error: {msg}", file=sys.stderr)
    def _load(self,paths):
        for p in paths:
            try:
                with open(p,'r',encoding='utf-8-sig') as f:
                    for raw in f:
                        line=strip_comment(raw).strip()
                        if not line: continue
                        self.lines.extend(q for q in map(str.strip,split_top(line,';')) if q)
            except OSError as e: self._error(f"cannot read '{p}': {e}"); return False
        return True
    def _decl_list(self,line):
        m=re.match(r'^('+_NAME+r')\s+(.+)$',line)
        if not m: return None
        t,rest=m.group(1),m.group(2).strip()
        if t not in TYPES and t not in self.objects and t!='joy': return None
        res=[]
        for part in split_top(rest,','):
            part=part.strip()
            if not part: continue
            mm=re.match(r'^('+_NAME+r')(?:\s*=\s*(.*))?$',part)
            if not mm: return None
            v=mm.group(2)
            res.append((t,mm.group(1),v.strip() if v else v))
        return res or None
    def parse(self):
        i=0
        while i < len(self.lines):
            line=self.lines[i]
            if line=='end': self._error("unexpected 'end' at top level"); i+=1
            elif line.startswith('object '): i=self._parse_object(i)
            elif line.startswith('function '): i=self._parse_function(i)
            elif self._decl_all(line): self._parse_global(line); i+=1
            else: self.top.append(line); i+=1
        return self.errors==0
    def _decl_all(self,line):
        lst=self._decl_list(line)
        if not lst: return None
        if any(t not in TYPES and t not in self.objects for t,n,v in lst): return None
        return lst
    def _obj_fields(self,name,fields,lst):
        for t,n,v in lst:
            if t not in TYPES: self._error(f"object '{name}': expected type")
            elif n in fields: self._error(f"dup field '{name}.{n}'")
            else: fields[n]=(t,v)
    def _parse_object(self,i):
        m=re.match(r'^object\s+('+_NAME+r')(?:\s+(.+))?$',self.lines[i])
        if not m: self._error(f"invalid object: {self.lines[i]}"); return i+1
        name,rest=m.group(1),(m.group(2) or '').strip()
        if name in self.objects: self._error(f"dup object '{name}'"); return i+1
        fields={}
        if rest and rest!='end':
            lst=self._decl_list(rest)
            if lst: self._obj_fields(name,fields,lst)
        j=i+1
        while j < len(self.lines):
            line=self.lines[j]
            if line=='end': self.objects[name]=fields; return j+1
            lst=self._decl_all(line)
            if not lst: self._error(f"object '{name}': expected 'type name = value', got {line}")
            else: self._obj_fields(name,fields,lst)
            j+=1
        self._error(f"object '{name}' no end"); return j
    def _parse_function(self,i):
        m=_FUNC_RE.match(self.lines[i]); name=m.group(1); params=self._parse_params(m.group(2) or ''); body,j=self._collect_block(i+1,f"function '{name}'")
        if name in self.functions: self._error(f"dup function '{name}'")
        else: self.functions[name]=(params,body)
        if any(line.startswith('return ') for line in body): self.func_ret[name]='num'
        return j
    def _infer_returns(self):
        for _ in range(4):
            ch=False
            for name in list(self.func_ret.keys()):
                params,body=self.functions[name]; saved=self.scope; self.scope={pn:pt for pt,pn in params}; kind='num'
                for line in body:
                    lst=self._decl_all(line)
                    if lst:
                        for t,n,v in lst:
                            if t in TYPES: self.scope[n]=t
                        continue
                    if line.startswith('return '):
                        if self.expr_type(line[7:].strip())=='str': kind='str'; break
                self.scope=saved
                if self.func_ret[name]!=kind: self.func_ret[name]=kind; ch=True
            if not ch: break
    def _parse_params(self,text):
        params=[]
        for part in split_top(text,',') if text.strip() else []:
            w=part.split()
            if len(w)!=2 or (w[0] not in TYPES and w[0] not in self.objects): self._error(f"invalid param '{part}'"); continue
            params.append((w[0],w[1]))
        return params
    def _collect_block(self,i,what):
        depth=0; body=[]
        while i < len(self.lines):
            line=self.lines[i]
            if line=='end':
                if depth==0: return body,i+1
                depth-=1
            elif line.startswith('if ') or line.startswith('loop '): depth+=1
            elif line=='else' or line.startswith('else if '):
                if depth==0: self._error(f"{what}: 'else' without 'if'"); return body,i+1
            elif line.startswith('object ') or line.startswith('function '): self._error(f"{what}: nested not allowed"); body.append(line); i+=1; continue
            body.append(line); i+=1
        self._error(f"{what} no end"); return body,i
    def _parse_global(self,line):
        for t,n,v in self._decl_all(line) or []:
            if n in self.vars: self._error(f"dup var '{n}'"); continue
            if t in self.objects:
                if not v or not re.match(r'^new\s+'+re.escape(t)+r'\s*\(\)?\s*$',v): self._error(f"'{n}': must be 'new {t}()'"); continue
            self.vars[n]=(t,v)
    def c_type(self,t):
        if t in TYPES: return TYPES[t]
        if t in self.objects: return t+' *'
        return 'double'
    def default_value(self,t): return 'NULL' if t=='str' else '0'
    def static_expr(self,v): return bool(_NUM_RE.match(v)) or (len(v)>=2 and v[0]=='"' and v[-1]=='"')
    def expr_type(self,expr):
        expr=expr.strip()
        if expr.startswith('"') and expr.endswith('"'): return 'str'
        m=re.match(r'^('+_NAME+r')\.('+_NAME+r')$',expr)
        if m:
            holder=m.group(1); ot=self.scope.get(holder) or (self.vars[holder][0] if holder in self.vars else None)
            fields=self.objects.get(ot)
            if fields and m.group(2) in fields: return fields[m.group(2)][0]
        if expr in self.scope: return self.scope[expr]
        if expr in self.vars: return self.vars[expr][0]
        call=re.match(r'^('+_NAME+r')\s*\(.*\)$',expr)
        if call:
            if call.group(1) in self.func_ret: return self.func_ret[call.group(1)]
            if call.group(1) in STR_BUILTINS: return 'str'
        return ENGINE_VARS.get(expr,'num')
    def expr(self,e):
        e=e.strip()
        if e=='true': return '1'
        if e=='false': return '0'
        parts=split_top(e,'+')
        if len(parts)>1 and all(parts) and any(self.expr_type(p)=='str' for p in parts):
            out=self.as_str(parts[0])
            for p in parts[1:]: out=f'ds_concat({out}, {self.as_str(p)})'
            return out
        return self._fields(e)
    def _fields(self,e):
        names=[n for n in self.vars if self.vars[n][0] in self.objects]+[n for n,t in self.scope.items() if t in self.objects]
        for n in sorted(names,key=len,reverse=True):
            e=sub_unquoted(e,re.compile(r'\b'+re.escape(n)+r'\.('+_NAME+r')'),n+r'->\1')
        return self._calls(e)
    def _calls(self,e):
        if not self.functions: return e
        _,quoted=scan(e); out=[]; start=0
        for m in re.finditer(r'\b('+_NAME+r')\s*\(',e):
            if quoted[m.start()] or m.group(1) not in self.functions: continue
            out.append(e[start:m.start()]); out.append('ds_fn_'+m.group(1)+'('); start=m.end()
        out.append(e[start:]); return ''.join(out)
    def as_str(self,e):
        if self.expr_type(e)=='str': return self.expr(e)
        return f'ds_num_to_string((double)({self.expr(e)}))'
    def _out(self,s): self.output.append('    '*self.indent+s)
    def _emit(self,s): self.output.append(s)
    def _emit_line(self,line):
        if line=='end':
            if not self.blocks: self._error("unexpected 'end'"); return
            self.blocks.pop(); self.indent-=1; self._out('}'); return
        if line.startswith('if '):
            self._open_block(f'if ({self.expr(strip_kw(line[3:].strip(),"then"))})'); return
        if line.startswith('loop '):
            self._open_block(f'while ({self.expr(strip_kw(line[5:].strip(),"do"))})'); return
        if line=='else' or line.startswith('else if '):
            if not self.blocks: self._error("'else' without 'if'"); return
            header='else' if line=='else' else f'else if ({self.expr(line[8:])})'
            self.indent-=1; self._out(f'}} {header} {{'); self.indent+=1; return
        if line=='return': self._out('return;'); return
        if line.startswith('return '): self._out(f'return {self.expr(line[7:])};'); return
        lst=self._decl_all(line)
        if lst and lst[0][0] in TYPES:
            for t,n,v in lst:
                if n in self.scope: self._error(f"dup var '{n}'"); continue
                self.scope[n]=t; init=f'= {self.expr(v)}' if v else f'= {self.default_value(t)}'
                self._out(f'{self.c_type(t)} {n} {init};')
            return
        self._emit_statement(line)
    def _open_block(self,header): self.blocks.append(header); self._out(header+' {'); self.indent+=1
    def _emit_statement(self,line):
        i=find_assign(line)
        if i>=0: self._emit_assign(line[:i].strip(), line[i+1:].strip()); return
        m=_CALL_RE.match(line)
        if not m: return
        name,rest=m.group(1),m.group(2) or ''; args=split_top(rest,',') if rest else []
        if name in self.functions:
            if len(args)!=len(self.functions[name][0]):
                self._error(f"call '{name}': {len(args)} argument(s), expected {len(self.functions[name][0])}")
                return
            fn=f'ds_fn_{name}'
        elif name in BUILTINS: fn=name
        else:
            # Раньше неизвестный вызов молча выбрасывался, и целая строка кода
            # (например, отправка своего состояния в сеть) исчезала из сборки.
            self._error(f"unknown function '{name}'")
            return
        self._out(f'{fn}({", ".join(self.expr(a) for a in args)});')
    def _emit_assign(self,lhs,rhs):
        m=_LHS_RE.match(lhs)
        if not m: return
        name,field=m.group(1),m.group(2)
        if rhs.startswith('new '): return
        if name not in self.scope and name not in self.vars and name not in ENGINE_VARS: return
        holder_type=self.scope.get(name) or self.vars.get(name,('',None))[0]
        if field and holder_type in self.objects: lhs=self._fields(lhs)
        self._out(f'{lhs} = {self.expr(rhs)};')
    def generate(self):
        self._infer_returns(); self.output=[]; self.indent=0
        self._emit('#include "runtime.h"'); self._emit('#include "net.h"'); self._emit('#include <math.h>'); self._emit('')
        for name in self.objects: self._emit(f'typedef struct {name} {name};')
        self._emit('')
        for name,fields in self.objects.items():
            self._emit(f'struct {name} {{')
            for f,(t,_v) in fields.items(): self._emit(f'    {self.c_type(t)} {f};')
            self._emit('};')
        self._emit('')
        for name in self.objects:
            self._emit(f'static {name} *ds_new_{name}(void);'); self._emit(f'static void ds_free_{name}({name} *self);')
        self._emit('')
        init_lines=[]
        for n,(t,v) in self.vars.items():
            if t in self.objects: self._emit(f'{t} *{n} = NULL;')
            elif v and self.static_expr(v): self._emit(f'{self.c_type(t)} {n} = {self.expr(v)};')
            else: self._emit(f'{self.c_type(t)} {n} = {self.default_value(t)};')
            # Ровно одно добавление: раньше объект с инициализатором попадал в
            # init_lines дважды, и ds_main() выделял Player/Enemy/Punch два
            # раза — первая копия утекала (видел LeakSanitizer в тестах).
            if v: init_lines.append(n)
        if self.vars: self._emit('')
        for n,(params,_b) in self.functions.items(): self._emit(f'static {self._ret_c(n)} ds_fn_{n}({self._params_c(params)});')
        if self.functions: self._emit('')
        for name,fields in self.objects.items():
            self._emit(f'static {name} *ds_new_{name}(void) {{'); self._emit(f'    {name} *self = ({name} *)calloc(1, sizeof(*self));')
            self._emit(f'    if (!self) {{ ds_runtime_error("out of memory: {name}"); return NULL; }}')
            for f,(t,v) in fields.items():
                if v: self._emit(f'    self->{f} = {self.expr(v)};')
            self._emit('    return self;'); self._emit('}'); self._emit(f'static void ds_free_{name}({name} *self) {{ free(self); }}'); self._emit('')
        for n,(params,body) in self.functions.items():
            self._emit(f'static {self._ret_c(n)} ds_fn_{n}({self._params_c(params)}) {{'); self.indent=1; self.scope={pn:pt for pt,pn in params}; self.blocks=[]; body_text='\n'.join(body)
            for _pt,pn in params:
                if not used_outside_strings(body_text,pn): self._out(f'(void){pn};')
            for line in body: self._emit_line(line)
            self._emit('}'); self._emit('')
        self._emit('static int ds_main(void) {'); self.indent=1; self.scope={}; self.blocks=[]
        for n in init_lines:
            t=self.vars[n][0]
            if t in self.objects: self._out(f'{n} = ds_new_{t}();')
            elif self.vars[n][1]:
                try: self._out(f'{n} = {self.expr(self.vars[n][1])};')
                except: pass
        for line in self.top: self._emit_line(line)
        self._emit('    return 0;'); self._emit('}'); self._emit('')
        self._emit('void reset(void) {'); self.indent=1
        for n,(t,v) in self.vars.items():
            if t in self.objects: self._out(f'if ({n}) ds_free_{t}({n});'); self._out(f'{n} = NULL;')
            elif t=='arr': self._out(f'if ({n}) arr_free({n});'); self._out(f'{n} = arr_new();')
            elif v and self.static_expr(v): self._out(f'{n} = {self.expr(v)};')
            else: self._out(f'{n} = {self.default_value(t)};')
        self._emit('}'); self._emit('')
        self._emit('void init(AAssetManager *assets) {'); self.indent=1; self._out('ds_set_asset_manager(assets);'); self._out('ds_main();')
        if 'init' in self.functions: self._out('ds_fn_init();')
        self._emit('}'); self._emit(''); self._emit('void update(void) {'); self.indent=1
        if 'update' in self.functions: self._out('ds_fn_update();')
        self._emit('}'); self._emit(''); self._emit('void draw(Buffer *buffer) {'); self.indent=1; self._out('(void)buffer;')
        if 'draw' in self.functions: self._out('ds_fn_draw();')
        self._emit('}'); self._emit(''); self._emit('void touch(float x, float y, int action, int pointer_id) {'); self.indent=1
        if 'touch' in self.functions:
            args=[]
            for i,(pt,_pn) in enumerate(self.functions['touch'][0]):
                if i>=4 or pt=='str': break
                args.append(f'({self.c_type(pt)}){("x","y","action","pointer_id")[i]}')
            self._out(f'ds_fn_touch({", ".join(args)});')
        else: self._out('(void)x; (void)y; (void)action; (void)pointer_id;')
        self._emit('}')
    def _ret_c(self,name): return self.c_type(self.func_ret[name]) if name in self.func_ret else 'void'
    def _params_c(self,params):
        if not params: return 'void'
        return ', '.join(f'{self.c_type(t)} {n}' for t,n in params)
    def compile(self,sources,output):
        if not self._load(sources): return False
        self.parse()
        try: self.generate()
        except Exception as exc: self._error(f"internal {exc}"); return False
        with open(output,'w',encoding='utf-8') as f: f.write('\n'.join(self.output)+'\n')
        return True
def main():
    output='game/game.c'; sources=[]; args=sys.argv[1:]; i=0
    while i < len(args):
        if args[i] in ('-o','--output') and i+1 < len(args): output=args[i+1]; i+=2
        else: sources.append(args[i]); i+=1
    if not sources: print("Usage: python ds_compiler.py file.ds [-o output.c]", file=sys.stderr); sys.exit(2)
    ok=DimScriptCompiler().compile(sources,output); print(f"{output}: {'OK' if ok else 'FAILED'}"); sys.exit(0 if ok else 1)
if __name__=='__main__': main()

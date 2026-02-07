import re
import sys

filepath = '/mnt/c/vpinball-master/retroeki_crash_reports/Game of Thrones LE (Stern 2015) VPW 1.2_20260207_121923_script.vbs'

keywords = {'if','elseif','while','until','for','select','case','dim','redim','set',
    'const','sub','function','class','property','with','each','in','to','step',
    'byval','byref','call','on','then','else','end','exit','do','loop','wend',
    'next','option','private','public','and','or','not','is','mod','like','xor',
    'eqv','imp','new','typeof','nothing','empty','null','true','false','type',
    'resume','goto','error','preserve','default','let','get','me'}

builtins = {'cbool','cbyte','ccur','cdate','cdbl','chr','cint','clng','csng','cstr',
    'date','err','hex','inputbox','instrrev','isarray','isdate','isempty','isnull',
    'isnumeric','isobject','join','lbound','lcase','left','len','log','ltrim','mid',
    'msgbox','now','oct','right','rtrim','sgn','space','split','sqr','strcomp',
    'string','strreverse','time','timer','trim','typename','ubound','ucase','vartype',
    'weekday','year','abs','array','asc','atn','cos','exp','fix','formatnumber','int',
    'instr','replace','rgb','rnd','round','sin','tan','eval','execute','executeglobal',
    'formatcurrency','formatdatetime','formatpercent','formatnumber','cstr'}

skip_words = keywords | builtins

with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
    lines = f.readlines()

for i, line in enumerate(lines, 1):
    stripped = line.strip()
    if stripped.startswith("'"):
        continue

    # Find all occurrences of: word SPACE (
    for m in re.finditer(r'\b(\w+)\s+\(', stripped):
        name = m.group(1)
        if name.lower() in skip_words:
            continue

        start = m.end() - 1  # position of (
        depth = 0
        pos = start
        found = False
        while pos < len(stripped):
            if stripped[pos] == '(':
                depth += 1
            elif stripped[pos] == ')':
                depth -= 1
                if depth == 0:
                    rest = stripped[pos+1:].lstrip()
                    if rest and rest[0] in '+-&*/\\^':
                        print(f'Line {i}: {stripped[:150]}')
                        found = True
                    break
            pos += 1
        if found:
            break

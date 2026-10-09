# 一次性诊断脚本：精确检查 C++ 源文件的花括号配平（跳过注释与字符串字面量）
# 为什么要它：MSVC 遇到未闭合 '{' 只会报 C1075 "no matching token found"，
# 然后把后面**所有**成员函数都报成 "class contains explicit override"，
# 错误信息离真正的问题位置很远。数括号能直接定位。
import sys

path = sys.argv[1]
src = open(path, encoding='utf-8').read()
i, n = 0, len(src)
depth = 0
line = 1
stack = []
BS = chr(92)  # 反斜杠，放变量里避免 heredoc 转义问题

while i < n:
    c = src[i]
    if c == '\n':
        line += 1
        i += 1
        continue
    if c == '/' and i + 1 < n and src[i + 1] == '/':
        while i < n and src[i] != '\n':
            i += 1
        continue
    if c == '/' and i + 1 < n and src[i + 1] == '*':
        i += 2
        while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
            if src[i] == '\n':
                line += 1
            i += 1
        i += 2
        continue
    if c == '"':
        i += 1
        while i < n and src[i] != '"':
            if src[i] == BS:
                i += 1
            i += 1
        i += 1
        continue
    if c == "'":
        # 字符字面量：跳过，避免 '\'' 被误判
        i += 1
        while i < n and src[i] != "'":
            if src[i] == BS:
                i += 1
            i += 1
        i += 1
        continue
    if c == '{':
        depth += 1
        stack.append(line)
    elif c == '}':
        depth -= 1
        if stack:
            stack.pop()
        if depth < 0:
            print('UNMATCHED } at line %d' % line)
            break
    i += 1

print('final depth = %d' % depth)
if depth > 0:
    print('unclosed { opened at lines: %s' % stack[:10])
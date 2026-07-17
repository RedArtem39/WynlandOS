import sys

def check_syntax(filename):
    with open(filename, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    stack = []
    errors = 0
    
    keywords_open = ['if', 'while', 'for', 'fn']
    
    for idx, line in enumerate(lines):
        line_num = idx + 1
        stripped = line.strip()
        
        # Skip comments
        if stripped.startswith('#') or not stripped:
            continue
        
        # Check matching of parentheses
        open_p = stripped.count('(')
        close_p = stripped.count(')')
        if open_p != close_p:
            print(f"Line {line_num}: Unbalanced parentheses ({open_p} open, {close_p} close): {stripped}")
            errors += 1
            
        open_b = stripped.count('[')
        close_b = stripped.count(']')
        if open_b != close_b:
            print(f"Line {line_num}: Unbalanced brackets ({open_b} open, {close_b} close): {stripped}")
            errors += 1
            
        # Parse blocks
        # We look for: fn name(...): or if ...: or while ...: or for ...:
        # and end
        first_word = stripped.split()[0]
        # Clean word from punctuation
        clean_word = "".join(c for c in first_word if c.isalnum())
        
        if clean_word in keywords_open:
            if stripped.endswith(':'):
                stack.append((clean_word, line_num))
            else:
                print(f"Line {line_num}: Block definition '{clean_word}' does not end with ':': {stripped}")
                errors += 1
        elif clean_word == 'elif' or clean_word == 'else':
            if not stripped.endswith(':'):
                print(f"Line {line_num}: '{clean_word}' does not end with ':': {stripped}")
                errors += 1
            # Check if there is an active if block on stack
            has_if = False
            for block, l in reversed(stack):
                if block == 'if':
                    has_if = True
                    break
            if not has_if:
                print(f"Line {line_num}: '{clean_word}' without matching 'if'")
                errors += 1
        elif clean_word == 'end':
            if not stack:
                print(f"Line {line_num}: 'end' without matching block")
                errors += 1
            else:
                block, l = stack.pop()
                # print(f"Closed {block} from line {l} at line {line_num}")
                
    if stack:
        for block, l in stack:
            print(f"Error: Unclosed block '{block}' defined at line {l}")
            errors += 1
            
    if errors == 0:
        print("Syntax check passed! No block matching or parentheses issues found.")
    else:
        print(f"Syntax check failed with {errors} errors.")

if __name__ == '__main__':
    check_syntax(sys.argv[1] if len(sys.argv) > 1 else 'browser.wyn')

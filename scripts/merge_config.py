#!/usr/bin/env python3
import sys
import os

def parse_toml(filepath):
    if not os.path.exists(filepath):
        return {}
    config = {}
    current_section = None
    with open(filepath, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or line.startswith(';'):
                continue
            if line.startswith('[') and line.endswith(']'):
                current_section = line[1:-1].strip()
                config[current_section] = {}
                continue
            if '=' in line:
                key, val = line.split('=', 1)
                key = key.strip()
                val = val.strip()
                if current_section is not None:
                    config[current_section][key] = val
    return config

def merge_configs(template_path, user_path, output_path):
    if not os.path.exists(template_path):
        print(f"Error: Template config not found at {template_path}")
        return False
        
    template_config = parse_toml(template_path)
    user_config = parse_toml(user_path)
    
    merged_lines = []
    current_section = None
    
    with open(template_path, 'r') as f:
        for line in f:
            stripped = line.strip()
            if stripped.startswith('[') and stripped.endswith(']'):
                current_section = stripped[1:-1].strip()
                merged_lines.append(line)
                continue
            if '=' in stripped and not stripped.startswith('#') and not stripped.startswith(';'):
                key, _ = stripped.split('=', 1)
                key = key.strip()
                
                if current_section in user_config and key in user_config[current_section]:
                    user_val = user_config[current_section][key]
                    indent = line[:len(line) - len(line.lstrip())]
                    merged_lines.append(f"{indent}{key} = {user_val}\n")
                else:
                    merged_lines.append(line)
                continue
            
            merged_lines.append(line)
            
    tmp_path = output_path + ".tmp"
    try:
        with open(tmp_path, 'w') as f:
            f.writelines(merged_lines)
        os.replace(tmp_path, output_path)
    except Exception as e:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        print(f"Error: Failed to write merged config atomically: {e}")
        return False
    print(f"[✓] Config merge completed: {user_path} updated with latest options.")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(1)
    merge_configs(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else sys.argv[2])

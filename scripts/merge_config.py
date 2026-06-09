#!/usr/bin/env python3
import sys
import os
import re

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
            # If section header
            if stripped.startswith('[') and stripped.endswith(']'):
                current_section = stripped[1:-1].strip()
                merged_lines.append(line)
                continue
            # If key-value pair
            if '=' in stripped and not stripped.startswith('#') and not stripped.startswith(';'):
                key, _ = stripped.split('=', 1)
                key = key.strip()
                
                # Check if user has a custom value for this key in the current section
                if current_section in user_config and key in user_config[current_section]:
                    user_val = user_config[current_section][key]
                    indent = line[:len(line) - len(line.lstrip())]
                    merged_lines.append(f"{indent}{key} = {user_val}\n")
                else:
                    merged_lines.append(line)
                continue
            
            # Preserve comments and blank lines
            merged_lines.append(line)
            
    # Write the merged file
    with open(output_path, 'w') as f:
        f.writelines(merged_lines)
    print(f"[✓] Config merge completed: {user_path} updated with latest options.")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: merge_config.py <template_path> <user_path> [output_path]")
        sys.exit(1)
    template = sys.argv[1]
    user = sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else user
    merge_configs(template, user, out)

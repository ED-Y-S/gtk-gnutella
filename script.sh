#!/bin/bash

# Simple script to change crlf to lf.
# To start: Put in gitignore folder and run.
# Note: ^.* files are not supported

ignore_name=()
ignore_path=()
ignore_extension=()
ignore_extension+=('.sh')
ignore_special=()

read_gitignore()
{
  filename=".gitignore"

  while IFS='' read -r line || [[ -n "$line" ]]; do
    [[ "$line" =~ ^#.*$ ]] && continue
    [[ "$line" =~ ^[[:space:]]*$ ]] && continue
    [[ "$line" =~ ^\*\. ]] && ignore_extension+=("${line:1}") && continue
    [[ "$line" =~ \/+ ]] && ignore_path+=("$line") && continue
    [[ "$line" =~ \* ]] && ignore_special+=("$line") && continue
    ignore_name+=("$line")
  done < "$filename"
}

# 0 = allow, 1 = ignore
check_ignore()
{
  type=$1
  
  if [ $1 == "directory" ]; then
    for dir in ${ignore_path[@]}
    do
      if [[ $2 =~ ^\./*"$dir" ]]; then
        return 1
      fi
    done
  elif [ $1 == "file" ]; then
    for ext in ${ignore_extension[@]}
    do
      if [ $ext == ".${2##*.}" ]; then
        return 1
      fi
    done
  fi
  
  for name in ${ignore_name[@]}
    do
      if [[ $2 =~ "$name"$ ]]; then
        return 1
      fi
    done
  
  for special in ${ignore_special[@]}
  do
    if [ $2 == "./$(echo $special)" ]; then
      return 1
    fi
  done
  
  return 0
}

crlf_to_lf()
{
  CURRENTPATH=$1
  
  for f in $CURRENTPATH
  do
    if [ -d "$f" ]; then
      if [ -L "$f" ]; then
        # Do nothing on symlink
        :
      else
        if check_ignore "directory" $f; then 
          crlf_to_lf "$f/*"
        fi
      fi
    else
      if check_ignore "file" $f; then 
        dos2unix "$f"
      fi
    fi
  done
}

read_gitignore
crlf_to_lf "./*"
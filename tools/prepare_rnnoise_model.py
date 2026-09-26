from pathlib import Path
import re
import sys


def main():
    source=Path(sys.argv[1]).read_text()
    result,count=re.subn(r"#ifndef DISABLE_DEBUG_FLOAT\n.*?#endif /\*DISABLE_DEBUG_FLOAT\*/","",source,flags=re.S)
    if count!=7:
        raise ValueError(f"Unexpected model layout: {count} optional debug arrays")
    Path(sys.argv[2]).write_text(result)


if __name__=="__main__":
    main()

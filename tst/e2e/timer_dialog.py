"""A modal export wait keeps due progress and completion callbacks alive."""
from session import Session

with Session("timer-dialog") as s:
    s.launch()
    s.logged("MODAL EXPORT COMPLETE")
    s.screenshot("exported", [(.1, .5, .9, .8, "40a060")])
    s.close()

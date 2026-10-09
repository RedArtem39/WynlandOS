Welcome to WynlandOS.

This is your home folder. Everything here belongs to you; the rest of
the system belongs to root.

  Files      the file manager (dock, or Alt+D)
  Terminal   Alt+Enter
  Log out    click your name in the bar

Administrators (the "wheel" group) become root with their own password:
  ary <cmd>     run one command as root
  ary su        a root shell ('exit' to leave)

Accounts:
  passwd                    change your password
  ary useradd -a NAME       a new administrator (without -a: a plain user)
  ary passwd NAME           set someone's password
  ary userdel -r NAME       remove an account and its home

Software: ary leaf install NAME (Ubuntu's packages), leaf search WORD.

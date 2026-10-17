# Login banner and prompt: which receiver, which OSIRIS, where on the
# network. Interactive shells only, so scp and ssh commands stay quiet.
[ -n "$PS1" ] || return 0

cat <<'EOF'

   ___  ____ ___ ____  ___ ____
  / _ \/ ___|_ _|  _ \|_ _/ ___|
 | | | \___ \| || |_) || |\___ \
 | |_| |___) | ||  _ < | | ___) |
  \___/|____/___|_| \_\___|____/
 ----------------------------------
EOF

osiris_state() {
	sed -n "s/^$1=//p" /data/osiris/state 2> /dev/null
}

printf "  %-9s %s %s (%s)\n" receiver "$(osiris_state make)" "$(osiris_state model)" \
	"$(osiris_state code)"
printf "  %-9s %s, slot %s\n" version "$(. /etc/os-release; echo $VERSION)" \
	"$(sed -n 's/.*osiris\.slot=\([ab]\).*/\1/p' /proc/cmdline)"
printf "  %-9s %s\n" mac "$(cat /sys/class/net/eth0/address 2> /dev/null)"
printf "  %-9s %s\n" ip "$(ifconfig eth0 2> /dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')"
printf "  %-9s %s\n\n" up "$(uptime | sed 's/^ *//')"

unset -f osiris_state
export PS1='\u@\h:\w\$ '

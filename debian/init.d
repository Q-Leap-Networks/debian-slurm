#!/bin/bash
#
# chkconfig: 345 90 10
# description: SLURM is a simple resource management system which \
#              manages exclusive access o a set of compute \
#              resources and distributes work to those resources.
#
# processname: /usr/sbin/slurmd 
# pidfile: /var/run/slurm/slurmd.pid
#
# processname: /usr/sbin/slurmctld
# pidfile: /var/run/slurm/slurmctld.pid
#
# config: /etc/default/slurm-llnl
#
### BEGIN INIT INFO
# Provides:          slurm
# Required-Start:    $local_fs $syslog $network $named munge
# Required-Stop:     $local_fs $syslog $network $named munge
# Default-Start:     3 5
# Default-Stop:      0 1 2 6
# Short-Description: slurm daemon management
# Description:       Start slurm to provide resource management
### END INIT INFO

BINDIR=/usr/bin
CONFDIR=/etc/slurm-llnl
LIBDIR=/usr/lib
SBINDIR=/usr/sbin

# Source slurm specific configuration
if [ -f /etc/default/slurm-llnl ] ; then
    . /etc/default/slurm-llnl
else
    SLURMCTLD_OPTIONS=""
    SLURMD_OPTIONS=""
fi

#Checking for configuration file
MISSING=""
for file in slurm.conf slurm.key slurm.cert ; do 
  if [ ! -f $CONFDIR/$file ] ; then
    MISSING="$MISSING $file"
  fi
done
if [ "${MISSING}" != "" ] ; then
  echo Not starting slurm-llnl
  echo $MISSING not found in $CONFDIR
  echo Please follow the instructions in \
  	/usr/share/doc/slurm-llnl/README.Debian
  exit 0
fi

test -f $BINDIR/scontrol || exit 0
for prog in $($BINDIR/scontrol show daemons) ; do 
  test -f $SBINDIR/$prog || exit 0
done

#Checking for lsb init function
if [ -f /lib/lsb/init-functions ] ; then
  . /lib/lsb/init-functions
else
  echo Can\'t find lsb init functions 
  exit 1
fi

# setup library paths for slurm and munge support
export LD_LIBRARY_PATH="$LIBDIR:$LD_LIBRARY_PATH"

get_daemon_description()
{
    case $1 in 
      slurmd)
        echo slurm compute node daemon
	;;
      slurmctld)
	echo slurm central management daemon
	;;
      *)
	echo slurm daemon
	;;
    esac
}

start() {
    desc="$(get_daemon_description $1)"
    log_daemon_msg "Starting $desc" "$1"
    unset HOME MAIL USER USERNAME 
    #FIXME $STARTPROC $SBINDIR/$1 $2
    local ERRORMSG="$(start-stop-daemon --start --oknodo \
    			--exec "$SBINDIR/$1" -- $2 2>&1)"
    STATUS=$?
    log_end_msg $STATUS
    if [ "$ERRORMSG" != "" ] ; then 
      echo $ERRORMSG
    fi
    touch /var/lock/slurm
}

stop() { 
    desc="$(get_daemon_description $1)"
    log_daemon_msg "Stopping $desc" "$1"
    local ERRORMSG="$(start-stop-daemon --oknodo --stop -s TERM \
    			--exec "$SBINDIR/$1" 2>&1)"
    STATUS=$?
    log_end_msg $STATUS
    if [ "$ERRORMSG" != "" ] ; then 
      echo $ERRORMSG
    fi
    rm -f /var/lock/slurm
}

startall() {
    for prog in `$BINDIR/scontrol show daemons`; do 
        optvar=`echo ${prog}_OPTIONS | tr "a-z" "A-Z"`
        start $prog ${!optvar} 
    done
}

#
# status() with slight modifications to take into account
# instantiations of job manager slurmd's, which should not be
# counted as "running"
#
slurmstatus() {
    local base=${1##*/}
    local pid
    local rpid
    local pidfile

    pidfile=`grep -i ${base}pid $CONFDIR/slurm.conf | grep -v '^ *#'`
    if [ $? = 0 ]; then
        pidfile=${pidfile##*=}
        pidfile=${pidfile%#*}
    else
        pidfile=/var/run/${base}.pid
    fi

    pid=`pidof -o $$ -o $$PPID -o %PPID -x $1 || \
         pidof -o $$ -o $$PPID -o %PPID -x ${base}`

    if [ -f $pidfile ]; then
        read rpid < $pidfile
        if [ "$rpid" != "" -a "$pid" != "" ]; then
            for i in $pid ; do
                if [ "$i" = "$rpid" ]; then 
                    echo $"${base} (pid $pid) is running..."
                    return 0
                fi     
            done
        elif [ "$rpid" != "" -a "$pid" = "" ]; then
#           Due to change in user id, pid file may persist 
#           after slurmctld terminates
            if [ "$base" != "slurmctld" ] ; then
               echo $"${base} dead but pid file exists"
            fi
            return 1
        fi 

    fi

    if [ "$base" = "slurmctld" -a "$pid" != "" ] ; then
        echo $"${base} (pid $pid) is running..."
        return 0
    fi
     
    echo $"${base} is stopped"
    
    return 3
}

#
# stop slurm daemons, 
# wait for termination to complete (up to 10 seconds) before returning
#
slurmstop() {
    for prog in `$BINDIR/scontrol show daemons`; do
       stop $prog
#       for i in 1 2 3 4
#       do
#          sleep $i
#          slurmstatus $prog
#          if [ $? != 0 ]; then
#             break
#          fi
#       done
    done
}

#
# The pathname substitution in daemon command assumes prefix and
# exec_prefix are same.  This is the default, unless the user requests
# otherwise.
#
# Any node can be a slurm controller and/or server.
#
case "$1" in
    start)
	startall
        ;;
    startclean)
        SLURMCTLD_OPTIONS="-c $SLURMCTLD_OPTIONS"
        SLURMD_OPTIONS="-c $SLURMD_OPTIONS"
        startall
        ;;
    stop)
	slurmstop
        ;;
    status)
	for prog in `$BINDIR/scontrol show daemons`; do
	   slurmstatus $prog
	done
        ;;
    restart)
        $0 stop
	sleep 1
        $0 start
        ;;
    force-reload)
        $0 stop
        $0 start
	;;
    condrestart)
        if [ -f /var/lock/subsys/slurm ]; then
            for prog in `$BINDIR/scontrol show daemons`; do
                 stop $prog
                 start $prog
            done
        fi
        ;;
    reconfig)
	for prog in `$BINDIR/scontrol show daemons`; do
	    killproc $prog -HUP
	done
	;;
    test)
	for prog in `$BINDIR/scontrol show daemons`; do   
	    echo "$prog runs here"
	done
	;;
    *)
        echo "Usage: $0 {start|startclean|stop|status|restart|reconfig|condrestart|test}"
        exit 1
        ;;
esac

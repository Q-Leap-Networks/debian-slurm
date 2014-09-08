#!/bin/bash
# Define location of slurm executables (if not in default search path)
#slurm_bin="/home/jette/slurm.way/bin/"

# Create private config file
# Set AuthType=auth/dummy
file_orig=`${slurm_bin}scontrol show config | awk '{ if ( $1 ~ /SLURM_CONFIG_FILE/ ) { print $3 } }'`
grep -iv AuthType <$file_orig >tmp.$$
echo "AuthType=auth/dummy" >>tmp.$$

# Run srun using this config file
export SLURM_CONF=tmp.$$
touch tmp.o.$$
${slurm_bin}srun /bin/id --output=tmp.o.$$

grep --quiet uid tmp.o.$$
if [ "$?" == 1 ] ; then
  echo "srun errors above are expected"
  echo "SUCCESS"
else
  echo "FAILURE"
fi

# Clean up
rm -f tmp.$$ tmp.o.$$

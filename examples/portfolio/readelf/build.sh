#!/usr/bin/env bash

# Make sure we exit if there is a failure
set -e

function usage() {
    echo "Usage: $0 [--with-musllvm] [--disable-inlining] [--ipdse] [--ai-dce] [--devirt VAL1] [--inter-spec VAL2] [--intra-spec VAL2] [--enable-config-prime] [--help]"
    echo "       VAL1=none|sea_dsa"    
    echo "       VAL2=none|aggressive|nonrec-aggressive|onlyonce"
}

#default values
INTER_SPEC="onlyonce"
INTRA_SPEC="onlyonce"
DEVIRT="sea_dsa"
OPT_OPTIONS=""
USE_MUSLLVM="false"

POSITIONAL=()
while [[ $# -gt 0 ]]
do
key="$1"
case $key in
    -inter-spec|--inter-spec)
	INTER_SPEC="$2"
	shift # past argument
	shift # past value
	;;
    -intra-spec|--intra-spec)
	INTRA_SPEC="$2"
	shift # past argument
	shift # past value
	;;
    -disable-inlining|--disable-inlining)
	OPT_OPTIONS="${OPT_OPTIONS} --disable-inlining"
	shift # past argument
	;;
    -enable-config-prime|--enable-config-prime)
	OPT_OPTIONS="${OPT_OPTIONS} --enable-config-prime"
	shift # past argument
	;;
    -with-musllvm|--with-musllvm)
	USE_MUSLLVM="true" 
	shift # past argument
	;;        
    -ipdse|--ipdse)
	OPT_OPTIONS="${OPT_OPTIONS} --ipdse"
	shift # past argument
	;;
    -ai-dce|--ai-dce)
	OPT_OPTIONS="${OPT_OPTIONS} --ai-dce"
	shift # past argument
	;;                    
    -devirt|--devirt)
	DEVIRT="$2"
	shift # past argument
	shift # past value
	;;        
    -help|--help)
	usage
	exit 0
	;;
    *)    # unknown option
	POSITIONAL+=("$1") # save it in an array for later
	shift # past argument
	;;
esac
done
set -- "${POSITIONAL[@]}" # restore positional parameters

#check that the require dependencies are built
if [ $USE_MUSLLVM == "true" ];
then
    declare -a bitcode=("readelf.bc" "libc.a.bc" "libc.a")
else
    declare -a bitcode=("readelf.bc")
fi    

for bc in "${bitcode[@]}"
do
    if [ -a  "$bc" ]
    then
        echo "Found $bc"
    else
	if [ "$bc" == "libc.a.bc" ];
	then
	    echo "Error: $bc not found. You need to compile musllvm and copy $bc to ${PWD}."
	else
            echo "Error: $bc not found. Try \"make\"."
	fi
        exit 1
    fi
done


MANIFEST=readelf.manifest

if [ $USE_MUSLLVM == "true" ];
then
    cat > ${MANIFEST} <<EOF    
{ "main" : "readelf.bc"
, "binary"  : "readelf"
, "modules"    : ["libc.a.bc"]
, "native_libs" : [ "/usr/lib/libiconv.dylib", "libc.a" ]
, "ldflags" : [ "-O2" ]
, "name"    : "readelf"
, "constraints" : [1, "readelf", "-s"]
}
EOF    
else
    cat > ${MANIFEST} <<EOF    
{ "main" : "readelf.bc"
, "binary"  : "readelf"
, "modules"    : []
, "native_libs" : [ "/usr/lib/libiconv.dylib" ]
, "ldflags" : [ "-O2" ]
, "name"    : "readelf"
, "constraints" : [1, "readelf", "-s"]
}
EOF
fi    
export OCCAM_LOGLEVEL=INFO
export OCCAM_LOGFILE=${PWD}/slash/occam.log

rm -rf slash

SLASH_OPTS="--inter-spec-policy=${INTER_SPEC} --intra-spec-policy=${INTRA_SPEC} --devirt=${DEVIRT} --no-strip --stats $OPT_OPTIONS"
echo "============================================================"
echo "Running readelf without libraries"
echo "slash options ${SLASH_OPTS}"
echo "============================================================"
slash ${SLASH_OPTS} --work-dir=slash ${MANIFEST}
status=$?
if [ $status -eq 0 ]
then
    ## runbench needs _orig and _slashed versions
    cp slash/readelf readelf_slashed
    cp binutils/binutils/readelf readelf_orig
else
    echo "Something failed while running slash"
fi    

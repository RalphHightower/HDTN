NAME="HDTN_SENDER"
DEST="HDTN_RECEIVER"
#get timestamp for logs
TIME=$(date "+%Y.%m.%d-%H.%M.%S")


#get custody type (none = 0, old = 1, new = 2)
CUSTODY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -c)
      if [[ $2 -eq 0 ]]; then
	      CUSTODY=0
      else
	      if (( $2 >= 0 && $2 <=1 )); then
		      CUSTODY="$2"
	      else
		      CUSTODY=0
	      fi
      fi
      shift
      shift
      ;;
    *)
      echo "Unknown argument $1 - use"
      echo "./$0"
      echo "./$0 0"
      echo "./$0 1"
      exit
      ;;
  esac
done

#kill existing HDTN
./kill.sh

if (( CUSTODY == 0 )); then
	CL="TCPCL_NO_CUSTODY"

	./run_hdtn_oneprocess_tcpcl &
	sleep 6
	./send_files

else
	CL="LTP_CUSTODY"
	echo "There is currently no process for sending files with custody for tcpcl"
	exit
	./run_hdtn_oneprocess_ltp_custody &
	sleep 6
	./send_files_custody

fi

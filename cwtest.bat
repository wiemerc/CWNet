;
; simple batch file for running a full functional test
;
; run this file on the Amiga with:
; cd AMIGA:CWNet
; execute cwtest.bat

; setup environment on the Unix side:
; cd ~/Programmieren/TFTP
; ./tftpd.py -d --datadir /Users/consti/var/run/tftp unix:///Users/consti/var/run/tftp/tftpd.sock
; socat ~/var/run/vbox/com1.sock ~/var/run/tftp/tftpd.sock

; mount handler
mount NET:
wait 5

; start transfer of a large file (with more than one 64KB buffer), and while this transfer
; is running (takes quite a while), start another transfer
; Note that currently the second file will fail with a timeout because socat on the Unix
; side will terminate when our TFTP server closes the connection.
; The transfered files will be stored in ~/var/run/tftp/incoming/
copy c:whdload net://127.0.0.1/whdload
copy devs:mountlist net://127.0.0.1/mountlist

; It seems we have to wait for the file transfers to finish, otherwise the serial interface
; gets disturbed and data gets lost (probably due to the chain handler -> Amiga Forever ->
; Windows -> Virtual Box -> socat -> TFTP server, wouldn't surprise me)
wait 120 

; list all files transfered
amiga:cwnet/listq
echo

; list one specific file
amiga:cwnet/listq whdload
echo

; try to list a file that does not exist
amiga:cwnet/listq xxx
echo

; unmount handler
amiga:cwnet/unmount NET:


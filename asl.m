%ASL Read eye position from an ASL eye tracker over TCP.
%   ASL(COMMAND, ...) sends COMMAND to the ASL MEX function. COMMAND is one
%   of the strings described below.
%
%   ASL('connect', ADDR) opens a TCP connection to the ASL eye tracker.
%   ADDR is a char array of the form 'addr:port', where addr is an IP
%   address or hostname. Once connected, a background thread reads eye
%   data from the tracker at 120Hz. On each pass it reads every complete
%   sample already waiting and keeps only the newest, so the saved sample
%   stays current even if the tracker sends data faster than 120Hz.
%   Raises an error if already connected, if ADDR is not in the form
%   'addr:port', or if the connection cannot be made.
%
%   If the background thread stops because of an error, such as the
%   tracker closing the connection, ASL disconnects itself and prints a
%   message giving the reason. You can call ASL('connect', ADDR) again
%   right away, without calling ASL('disconnect') first.
%
%   ASL('read') displays the most recent eye data sample saved by the
%   background thread. It does not wait for new data. Each sample
%   contains these fields:
%
%       status                    0 = normal, 1 = calibration
%       calibration_point_number  Calibration point number
%       frame_number              Frame number from the tracker
%       pupil_diameter            Pupil diameter
%       gaze_x, gaze_y            Gaze position
%
%   Raises an error if not connected, or if no sample has been received
%   since connecting.
%
%   ASL('disconnect') stops the background thread and closes the
%   connection. Raises an error if not connected.
%
%   Example:
%       asl('connect', '128.120.140.228:51000');
%       asl('read');
%       asl('disconnect');
%
%   ASL is a MEX function. Build it with:
%       mex asl.cpp
%
%   This file contains only help text. When asl.mexw64 is on the path,
%   MATLAB runs the MEX file and uses this file for HELP ASL.

% This file is not executed when the MEX file is present.
error('asl:notCompiled', 'The asl MEX file was not found. Build it with: mex asl.cpp');

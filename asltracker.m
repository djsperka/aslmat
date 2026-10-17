classdef asltracker < handle
%ASLTRACKER Connection to an ASL eye tracker.
%   T = ASLTRACKER('address', ADDR, 'screen_rect', RECT) creates a tracker
%   object and connects to the ASL eye tracker. Once connected, the asl
%   MEX file reads eye data in the background at 120Hz. All arguments are
%   required:
%
%       'address'            Tracker address, a char array of the form
%                            'addr:port', where addr is an IP address or
%                            hostname.
%       'screen_rect'        Screen rect in pixels, [left top right bottom],
%                            as returned by Screen('Rect', window).
%
%   T = ASLTRACKER(..., 'parport_address', PORT) sets the I/O address of
%   the parallel port used to send XDAT values to the tracker. The default
%   is 0x4EFC. The port is opened with IO64Device, so cclab-matlab-tools
%   must be on the path.
%
%   T = ASLTRACKER(..., 'initial_port_value', VALUE) sets the XDAT value
%   written to the parallel port when the object is created. VALUE is an
%   integer from 0 to 255. The default is 0.
%
%   T = ASLTRACKER(..., 'data_folder', FOLDER) sets the folder where
%   Eye-Trac writes its .eyd data files. START_RECORDING checks this
%   folder for a new file. The default is
%   'C:\Users\Public\Documents\Applied Science Laboratories\Eye-Trac 6 data'.
%
%   T = ASLTRACKER(..., 'new_file_time', SECONDS) sets how long
%   START_RECORDING waits for Eye-Trac to open a new data file before
%   checking. The default is 0.5.
%
%   Raises an error if an argument is missing or invalid, if the parallel
%   port cannot be opened, if a connection is already open, or if the
%   connection cannot be made.
%
%   XDAT is an 8-bit value written to the parallel port. Bits 0-5 hold the
%   value from MESSAGE, and bits 6-7 are set by START_RECORDING and cleared
%   by STOP_RECORDING.
%
%   ASLTRACKER methods:
%       eyepos     - Return the most recent eye data sample as a struct
%       calibrate  - Follow the tracker through a calibration
%       message    - Send a message value (bits 0-5 of XDAT)
%       start_recording - Set the recording bits (6-7 of XDAT) and send
%       stop_recording  - Clear the recording bits (6-7 of XDAT) and send
%       disconnect - Close the connection
%       connect    - Reconnect after DISCONNECT, or after the tracker
%                    dropped the connection
%
%   ASLTRACKER uses the asl MEX file, which supports one connection at a
%   time, so only one ASLTRACKER object can be connected at once.
%
%   Deleting the object, or clearing the last variable that refers to it,
%   closes its connection.
%
%   Example:
%       t = asltracker('address', '128.120.140.228:51000', ...
%           'screen_rect', [0 0 1920 1080]);
%       s = t.eyepos(2);    % wait up to 2 s for the first sample
%       fprintf('gaze = (%g, %g)\n', s.gaze_x, s.gaze_y);
%       t.disconnect();
%
%   See also ASL.

    properties (SetAccess = private)
        Address (1,:) char      % Tracker address, 'addr:port'
        ScreenRect              % Screen rect in pixels, [left top right bottom]
        DataFolder (1,:) char   % Folder where Eye-Trac writes .eyd data files
        NewFileTime (1,1) double  % Seconds START_RECORDING waits before checking for a new data file
        DataFile (1,:) char = ''  % Full path of the data file opened by the last START_RECORDING
        GazeToPixels (2,2)      % Convert gaze coordinates to pixels with this
        GazeOffset (1,2)        % offset of gaze position in pixel coords
    end

    properties (Access = private)
        OwnsConnection (1,1) logical = false    % true if this object opened the current connection
        IODevice                                % IO64Device used to send XDAT over the parallel port
        Xdat (1,1) uint8                        % Value last sent to the parallel port. Bits 0-5: message, bits 6-7: recording
        AlwaysInRect (1,1) logical = false      % If set, is_in_rect will always return true
    end

    properties (Constant, Access = private)
        MessageMask = uint8(0x3F)               % Xdat bits 0-5
        RecordingMask = uint8(0xC0)             % Xdat bits 6-7
        GazeSystemDimensions = [260, 255]       % Size of ASL gaze coord system
    end

    methods
        function obj = asltracker(options)
            arguments
                options.address (1,:) char {mustBeNonempty}
                options.screen_rect (1,4) {mustBeNumeric, mustBeReal, mustBeFinite, mustBeValidRect}
                options.parport_address (1,1) {mustBeNumeric, mustBeInteger, mustBeNonnegative} = 0x4EFC
                options.initial_port_value (1,1) {mustBeNumeric, mustBeInteger, mustBeInRange(options.initial_port_value, 0, 255)} = 0
                options.data_folder (1,:) char {mustBeNonempty} = 'C:\Users\Public\Documents\Applied Science Laboratories\Eye-Trac 6 data'
                options.new_file_time (1,1) double {mustBeNonnegative, mustBeFinite} = 0.5
            end
            required = {'address', 'screen_rect'};
            missing = required(~isfield(options, required));
            if ~isempty(missing)
                error('asltracker:missingArgument', ...
                    'asltracker requires these arguments: %s. Missing: %s.', ...
                    strjoin(required, ', '), strjoin(missing, ', '));
            end
            obj.Address = options.address;
            obj.ScreenRect = double(options.screen_rect);
            obj.DataFolder = options.data_folder;
            obj.NewFileTime = options.new_file_time;
            obj.IODevice = IO64Device(double(options.parport_address));
            obj.Xdat = uint8(options.initial_port_value);
            obj.GazeToPixels = [RectWidth(obj.ScreenRect)/obj.GazeSystemDimensions(1), 0; 0, RectHeight(obj.ScreenRect)/obj.GazeSystemDimensions(2)];
            obj.GazeOffset = obj.ScreenRect(1:2);

            obj.sendXdat();

            obj.connect();
        end

        function connect(obj)
            %CONNECT Reconnect to the tracker.
            %   T.CONNECT() connects to the tracker at T.Address. The
            %   constructor connects, so you only need this after
            %   DISCONNECT, or after the tracker dropped the connection.
            %   Raises an error if a connection is already open or the
            %   connection cannot be made.
            asl('connect', obj.Address);
            obj.OwnsConnection = true;
        end

        function s = eyepos(obj, timeout)
            %EYEPOS Return the most recent eye data sample.
            %   S = T.EYEPOS() returns the most recent sample as a struct
            %   with fields status, calibration_point_number, frame_number,
            %   pupil_diameter, gaze_x and gaze_y. All values are doubles.
            %   The gaze coordinates are converted to screen pixels.
            %
            %   S = T.EYEPOS(TIMEOUT) waits up to TIMEOUT seconds for the
            %   first sample to arrive after connecting. The default is 0,
            %   which raises an error at once if no sample has arrived yet.
            arguments
                obj
                timeout (1,1) double {mustBeNonnegative} = 0
            end
            t = tic;
            while true
                try
                    s = asl('eyepos');
                    gazePixels = [s.gaze_x, s.gaze_y] * obj.GazeToPixels + obj.GazeOffset;
                    s.gaze_x = gazePixels(1);
                    s.gaze_y = gazePixels(2);
                    return;
                catch e
                    if ~contains(e.message, 'No eye data received yet') || toc(t) >= timeout
                        rethrow(e);
                    end
                end
                pause(0.01);
            end
        end

        function [tf] = is_in_rect(obj, rect)
            %is_in_rect Is current eye pos in the rect? Returns 1/0.

            arguments
                obj
                rect (1,4) {mustBeNumeric, mustBeReal, mustBeFinite, mustBeValidRect}
            end

            if obj.AlwaysInRect
                tf = true;
            else
                s = obj.eyepos();
                x = s.gaze_x;
                y = s.gaze_y;
                tf = IsInRect(x, y, rect);
            end
        end

        function always_in_rect(obj, bAlways)
            obj.AlwaysInRect = bAlways;
            if bAlways
                fprintf('WARNING: eyetracker is in "always looking" mode! Actual eye position is ignored.\n');
            else
                fprintf('WARNING: "always looking" mode turned off! Actual eye position is used.\n');
            end
        end

        function calibrate(obj, window, fixpt, timeout)
            %CALIBRATE Follow the tracker through a calibration.
            %   T.CALIBRATE(WINDOW, FIXPT) waits until the tracker reports
            %   a non-zero calibration point number, then follows the
            %   calibration as it moves from point to point. Each time the
            %   point changes, FIXPT is moved to the new point, drawn in
            %   WINDOW and shown. WINDOW is a Psychtoolbox window pointer
            %   and FIXPT is a FixationPoint. Returns when the calibration
            %   point number goes back to 0.
            %
            %   T.CALIBRATE(WINDOW, FIXPT, TIMEOUT) waits up to TIMEOUT
            %   seconds for the calibration point number to become
            %   non-zero, and raises an error if it doesn't. The default
            %   is 10.
            arguments
                obj
                window (1,1) double
                fixpt (1,1) FixationPoint
                timeout (1,1) double {mustBeNonnegative} = 10
            end

            % clear screen
            Screen('Flip', window);

            % Poll at the rate the asl background thread reads samples.
            period = 1/120;

            % Wait for the tracker to start calibrating. The timeout also
            % covers waiting for the first sample after connecting.
            t = tic;
            s = obj.eyepos(timeout);
            while s.calibration_point_number == 0
                if toc(t) >= timeout
                    error('asltracker:calibrationTimeout', ...
                        'Calibration did not start within %g seconds.', timeout);
                end
                pause(period);
                s = obj.eyepos();
            end

            % Follow the calibration. The first point counts as a change.
            currentPoint = 0;
            while s.calibration_point_number ~= 0
                if s.calibration_point_number ~= currentPoint
                    currentPoint = s.calibration_point_number;
                    targetPosition = [s.gaze_x, s.gaze_y];
                    targetPixels = obj.GazeToPixels * targetPosition(:) + obj.GazeOffset(:);
                    fprintf('cal pt %d: %f, %f --> %f, %f\n', currentPoint, s.gaze_x, s.gaze_y, targetPixels(1), targetPixels(2));

                    % Move the fixation point to the target and show it.
                    % FixationPoint is a value class, so assign the result back.
                    fixpt.XY = targetPixels(:)';
                    fixpt.draw(window);
                    Screen('Flip', window);
                end
                pause(period);
                s = obj.eyepos();
            end
            % leave a blank screen when done
            Screen('Flip', window);
        end

        function message(obj, value)
            %MESSAGE Send a message value to the tracker.
            %   T.MESSAGE(VALUE) rounds VALUE to an integer and puts its
            %   bits 0-5 in bits 0-5 of the XDAT value, then writes XDAT to
            %   the parallel port. Higher bits of VALUE are ignored, and
            %   bits 6-7 of XDAT (the recording bits) are unchanged.
            %   Negative values are treated as two's complement.
            arguments
                obj
                value (1,1) {mustBeNumeric, mustBeReal, mustBeFinite}
            end
            % int64 rounds; typecast keeps the two's complement bits of negative values.
            bits = typecast(int64(value), 'uint64');
            low6 = uint8(bitand(bits, uint64(obj.MessageMask)));
            obj.Xdat = bitor(bitand(obj.Xdat, obj.RecordingMask), low6);
            obj.sendXdat();
        end

        function start_recording(obj)
            %START_RECORDING Tell the tracker to start recording.
            %   T.START_RECORDING() sets bits 6 and 7 of the XDAT value,
            %   leaving the message bits unchanged, and writes XDAT to the
            %   parallel port. It then waits T.NewFileTime seconds and
            %   checks that Eye-Trac has opened a new .eyd file in
            %   T.DataFolder. The new file's full path is saved in
            %   T.DataFile.
            %
            %   Raises an error if T.DataFolder doesn't exist (before
            %   anything is sent), or if no new file appears. In the
            %   second case the recording bits have already been sent, so
            %   call STOP_RECORDING before trying again.
            if ~isfolder(obj.DataFolder)
                error('asltracker:noDataFolder', ...
                    'Data folder does not exist: %s', obj.DataFolder);
            end
            obj.DataFile = '';
            before = obj.listDataFiles();

            obj.Xdat = bitor(obj.Xdat, obj.RecordingMask);
            obj.sendXdat();

            pause(obj.NewFileTime);
            after = obj.listDataFiles();
            [newNames, idx] = setdiff({after.name}, {before.name});
            if isempty(newNames)
                error('asltracker:noNewDataFile', ...
                    'No new data file appeared in %s within %g seconds of starting recording.', ...
                    obj.DataFolder, obj.NewFileTime);
            end
            % If more than one appeared, take the newest.
            newFiles = after(idx);
            [~, newest] = max([newFiles.datenum]);
            obj.DataFile = fullfile(obj.DataFolder, newFiles(newest).name);
        end

        function stop_recording(obj)
            %STOP_RECORDING Tell the tracker to stop recording.
            %   T.STOP_RECORDING() clears bits 6 and 7 of the XDAT value,
            %   leaving the message bits unchanged, and writes XDAT to the
            %   parallel port.
            obj.Xdat = bitand(obj.Xdat, obj.MessageMask);
            obj.sendXdat();
        end

        function disconnect(obj)
            %DISCONNECT Close the connection.
            %   T.DISCONNECT() stops reading eye data and closes the
            %   connection. Raises an error if not connected.
            obj.OwnsConnection = false;
            asl('disconnect');
        end

        function delete(obj)
            % Close the connection if this object opened it. It may already
            % be closed, e.g. if the tracker dropped it.
            if obj.OwnsConnection
                try
                    asl('disconnect');
                catch
                end
            end
        end
    end

    methods (Access = private)
        function sendXdat(obj)
            % XDAT goes over the parallel port; sending it over TCP/IP
            % (asl('xdat')) doesn't work with the tracker.
            obj.IODevice.outp(double(obj.Xdat));
        end

        function files = listDataFiles(obj)
            % Eye-Trac data files in DataFolder.
            files = dir(fullfile(obj.DataFolder, '*.eyd'));
            files = files(~[files.isdir]);
        end
    end
end

function mustBeValidRect(rect)
    % A rect is [left top right bottom], with right > left and bottom > top.
    if rect(3) <= rect(1) || rect(4) <= rect(2)
        error('asltracker:invalidRect', ...
            'screen_rect must be [left top right bottom], with right > left and bottom > top.');
    end
end

% Run a calibration on the ASL tracker. 
% This script will open a window on the Datapixx screen, create a fixation
% point object, connect to the eye tracker, and will display the target
% points requested by the ASL during calibration.
%
% Once the calibration is initiated, the operator must start the
% calibration process from the ASL gui program. Do NOT start it before the
% prompt tells you do do so. 

% open datapixx
if ~Datapixx('Open')
    error('Could not open Datapixx device.');
end

% open full screen window
[window,window_rect,big_window_rect] = makeWindow([], 2);

% Datapixx setup. After issuing commands, must call 'RegWr'
% enable research mode
Datapixx('EnableResearchMode');
Datapixx('SetDisplayGammaMode', 1); %1 = linear; 2 = 2.2
Datapixx('RegWrRd');       

% Check mode
status = Datapixx('GetVideoStatus');
gammaMode = Datapixx('GetDisplayGammaMode');  %1 = linear; 2 = 2.2       

% Verify the display is in research mode before continuing
if ~status.viewpixx3OLEDResearchMode || gammaMode~=1
    error('Datapixx is not in Research Mode, or is not linear, or both.');
end

% create tracker object
tracker=asltracker('address', '128.120.140.228:51000', 'screen_rect', window_rect);

% create fixation point for calibration
fixpt=FixationPoint([0,0], 50, [1,0,0], 'o');

% Wait for user input, then start calibration process. 
% User must open 'Custom Calibration' after hitting enter. 
input('Press Enter, then open Custom Calibration...\n', 's');
tracker.calibrate(window, fixpt, 30);

% Disable Research Mode, restore gamma to normal
Datapixx('DisableResearchMode');
Datapixx('SetDisplayGammaMode', 2); %1 = linear; 2 = 2.2
Datapixx('RegWr');        

Datapixx('Close');
sca
tracker.disconnect();
clear tracker;

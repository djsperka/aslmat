# aslmat

MATLAB mex file for monitoring eye position using the ASL eye tracker. 

## compilation

> [!NOTE]
> Need to have boost installed (along with 64 bit libs). Use include folder (folder above boost/*.hpp) and libs folder in mex command.

TODO: REmoved most of boost stuff, as the tcpip communication stuff is way too complicated. Winsock isn't much better.

`mex asl.cpp -ID:\boost\boost_1_82_0`

# aslmat

MATLAB mex file for monitoring eye position using the ASL eye tracker. 

## compilation

> [!NOTE]
> Need to have boost installed (along with 64 bit libs). Use include folder (folder above boost/*.hpp) and libs folder in mex command.

`mex asl.cpp -ID:\boost\boost_1_82_0 -LD:\boost\boost_1_82_0\lib64-msvc-14.3`

' Test: Array property access on external objects should be transformed
' Expected patterns after transformation:
'   + LightArray\(i\)\.State
'

' Table script using external class arrays
Dim i, LightArray(10)
For i = 0 To UBound(LightArray)
    LightArray(i).State = 1
    LightArray(i).intensityscale = 0.5
Next

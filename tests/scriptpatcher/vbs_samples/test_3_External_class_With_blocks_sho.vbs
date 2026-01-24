' Test: External class With blocks should be transformed
' Expected patterns after transformation:
'   + Set DTBank = cvpmDropTarget_Create\(\)
'   + cvpmDropTarget_InitDrop\s+DTBank,\s*Array\(sw41, sw42\),\s*Array\(41, 42\)
'   + cvpmDropTarget_InitSnd\s+DTBank,\s*\"dropsound\",\s*\"raisesound\"
' Should NOT contain:
'   - With DTBank
'   - \.InitDrop
'

' External script that uses cvpmDropTarget from core.vbs
Dim DTBank
Set DTBank = New cvpmDropTarget
With DTBank
    .InitDrop Array(sw41, sw42), Array(41, 42)
    .InitSnd "dropsound", "raisesound"
End With

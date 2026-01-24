' Test: External class method calls without With should be transformed
' Expected patterns after transformation:
'   + cvpmDropTarget_Hit\s+DTBank,\s*1
'   + cvpmDropTarget_SolHit\s+DTBank,\s*2,\s*True
' Should NOT contain:
'   - DTBank\.Hit
'   - DTBank\.SolHit
'

' External script with method calls
Dim DTBank
Set DTBank = New cvpmDropTarget
DTBank.Hit 1
DTBank.SolHit 2, True

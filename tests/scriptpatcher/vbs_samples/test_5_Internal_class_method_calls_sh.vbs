' Test: Internal class method calls should be transformed
' Expected patterns after transformation:
'   + cvpmDropTarget_CheckAllDn\s+this_,\s*True
' Should NOT contain:
'   - \bCheckAllDn\s+True\b(?!.*this_)
'

Class cvpmDropTarget
    Private mSw

    Public Sub Hit(aNo)
        CheckAllDn True
    End Sub

    Private Sub CheckAllDn(aStatus)
        ' Do something
    End Sub
End Class

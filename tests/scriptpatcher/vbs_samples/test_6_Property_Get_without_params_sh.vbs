' Test: Property Get without params should be transformed inside class
' Expected patterns after transformation:
'   + FlipperPolarity_Get_StartPoint\(this_\)
'   + FlipperPolarity_Get_EndPoint\(this_\)
'

Class FlipperPolarity
    Private mStartPoint
    Private mEndPoint

    Public Property Get StartPoint
        StartPoint = mStartPoint
    End Property

    Public Property Get EndPoint
        EndPoint = mEndPoint
    End Property

    Public Sub Calculate()
        Dim diff
        diff = EndPoint - StartPoint
    End Sub
End Class
